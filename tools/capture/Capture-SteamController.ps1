<#
.SYNOPSIS
  Captures the Steam Controller (2026) USB traffic needed to enable the sc26 profile.

.DESCRIPTION
  Drives USBPcapCMD.exe and tshark.exe from the command line so neither the Wireshark GUI nor
  Npcap is needed (the silent Wireshark install skips Npcap; USBPcap does not use it). Two phases:

    -Phase Plugin   Capture 1 of docs/STEAM_CONTROLLER_PROFILE.md §7.2: starts a capture on every
                    USBPcap root hub, waits for the controller to be plugged in (polls PnP for
                    VID_28DE), records ten more seconds, stops, keeps the hub that saw the device
                    descriptor, and writes
                      captures/sc26-plugin.pcapng, captures/sc26-plugin.json,
                      captures/sc26-plugin-summary.txt (idProduct, bcdDevice, interfaces),
                      captures/sc26-report-descriptor-<n>.hex (one per HID interface),
                      captures/steam-controller-hid.txt (Get-SteamControllerHid.ps1 output),
                      captures/usbpcap-interface.txt (the hub, reused by -Phase Steam).

    -Phase Steam    Capture 2 of §7.3: controller already plugged in, Steam not running. Starts
                    the capture on the hub from phase 1 (descriptors of already-connected devices
                    are injected), tells you to start Steam and exercise every input, waits for
                    Enter, stops, and writes
                      captures/sc26-steam.pcapng, captures/sc26-steam.json,
                      captures/sc26-steam-control.tsv (every control transfer: SET_REPORT /
                      GET_REPORT with wValue, wIndex and payload),
                      captures/sc26-steam-interrupt.tsv (every interrupt transfer: time, endpoint,
                      payload) and captures/sc26-steam-summary.txt (report ids seen, counts).

  Requirements: USBPcap installed and the machine rebooted since (the filter attaches to the root
  hubs at boot; `\\.\USBPcapN` must exist), Wireshark installed (for tshark.exe), run from the
  repository root in an *elevated* PowerShell (USBPcapCMD needs it). Plain USB-C cable: not the
  puck, not Bluetooth.

.EXAMPLE
  .\tools\capture\Capture-SteamController.ps1 -Phase Plugin
  .\tools\capture\Capture-SteamController.ps1 -Phase Steam
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Plugin', 'Steam')]
    [string] $Phase,

    [string] $OutputDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'captures'),
    [string] $VendorId = '28DE',
    [string] $UsbPcapCmd = 'C:\Program Files\USBPcap\USBPcapCMD.exe',
    [string] $TShark = 'C:\Program Files\Wireshark\tshark.exe',
    # Seconds to keep recording after the controller enumerates (Plugin phase).
    [int] $SettleSeconds = 10
)

$ErrorActionPreference = 'Stop'
$InformationPreference = 'Continue'

# --- console control: send Ctrl+C to USBPcapCMD's own console so it closes its pcap cleanly ---
# Done from a throwaway PowerShell process: attaching to another console requires detaching from
# our own, which would cut this script off from the user's window.
$script:CtrlCHelper = @'
param([int] $TargetPid)
Add-Type -Namespace Capture -Name ConsoleCtrl -MemberDefinition @"
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool AttachConsole(uint dwProcessId);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
[DllImport("kernel32.dll", SetLastError = true)] public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);
"@
[Capture.ConsoleCtrl]::FreeConsole() | Out-Null
if (-not [Capture.ConsoleCtrl]::AttachConsole([uint32] $TargetPid)) { exit 2 }
[Capture.ConsoleCtrl]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
if (-not [Capture.ConsoleCtrl]::GenerateConsoleCtrlEvent(0, 0)) { exit 3 }
Start-Sleep -Milliseconds 200
exit 0
'@

function Stop-ConsoleProcess {
    param([System.Diagnostics.Process] $Process, [int] $TimeoutSeconds = 10)
    if ($Process.HasExited) { return }
    $helper = Join-Path $env:TEMP "sc26-ctrlc-$PID.ps1"
    Set-Content -Path $helper -Value $script:CtrlCHelper -Encoding ascii
    $h = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $helper, '-TargetPid', $Process.Id) -WindowStyle Hidden -PassThru -Wait
    $sent = ($h.ExitCode -eq 0)
    if (-not $sent) { Write-Warning "Ctrl+C helper failed (exit $($h.ExitCode)) for pid $($Process.Id)." }
    if ($sent -and $Process.WaitForExit($TimeoutSeconds * 1000)) { return }
    Write-Warning "USBPcapCMD (pid $($Process.Id)) did not stop on Ctrl+C; killing it. The last few packets may be lost."
    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    $Process.WaitForExit(5000) | Out-Null
}

function Test-IsElevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-UsbPcapInterfaces {
    $found = @()
    foreach ($n in 1..16) {
        $name = "\\.\USBPcap$n"
        try {
            $h = [System.IO.File]::Open($name, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            $h.Close()
            $found += $name
        } catch { }
    }
    return $found
}

function Start-UsbCapture {
    param([string] $Interface, [string] $OutFile, [switch] $InjectDescriptors)
    $args = @('-d', $Interface, '-o', $OutFile, '-A')
    if ($InjectDescriptors) { $args += '--inject-descriptors' }
    $p = Start-Process -FilePath $UsbPcapCmd -ArgumentList $args -PassThru -WindowStyle Minimized
    Start-Sleep -Milliseconds 500
    if ($p.HasExited) { throw "USBPcapCMD exited immediately for $Interface (exit $($p.ExitCode)). Is this an elevated shell, and has the machine been rebooted since USBPcap was installed?" }
    return $p
}

function Invoke-TShark {
    param([string[]] $Arguments)
    $out = & $TShark @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "tshark failed ($LASTEXITCODE): $($Arguments -join ' ')`n$out" }
    return $out
}

function Test-ControllerPresent {
    [bool] (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match "USB\\VID_$VendorId" } | Select-Object -First 1)
}

# Frames carrying a device descriptor from our vendor -> device address on that hub.
function Find-ControllerAddress {
    param([string] $Pcap)
    $rows = Invoke-TShark @('-r', $Pcap, '-Y', "usb.idVendor == 0x$VendorId", '-T', 'fields', '-e', 'usb.device_address', '-e', 'usb.idProduct', '-e', 'usb.bcdDevice', '-e', 'usb.bNumInterfaces', '-E', 'separator=|')
    foreach ($row in @($rows)) {
        $f = "$row" -split '\|'
        if ($f.Count -ge 3 -and $f[0]) { return [pscustomobject]@{ Address = [int] $f[0]; ProductId = $f[1]; BcdDevice = $f[2] } }
    }
    return $null
}

# Raw payload of a frame, as a hex string, without the USBPcap pseudo-header (whose first two
# bytes are its own length). Used when the dissector hands the descriptor to usbhid and does not
# expose it as usb.control.Response.
function Get-FramePayloadHex {
    param([string] $Pcap, [int] $FrameNumber)
    $dump = Invoke-TShark @('-r', $Pcap, '-Y', "frame.number == $FrameNumber", '-x')
    $bytes = New-Object System.Collections.Generic.List[string]
    foreach ($line in @($dump)) {
        # "0000  1b 00 10 60 ..."  (stop at the first blank line: later blocks are reassembled views)
        if ("$line" -match '^[0-9a-f]{4}\s+((?:[0-9a-f]{2}\s?)+)') {
            foreach ($b in ($Matches[1].Trim() -split '\s+')) { if ($b) { $bytes.Add($b) } }
        } elseif ($bytes.Count -gt 0 -and "$line".Trim() -eq '') { break }
    }
    if ($bytes.Count -lt 2) { return '' }
    $headerLen = [Convert]::ToInt32($bytes[0], 16) + ([Convert]::ToInt32($bytes[1], 16) -shl 8)
    return (($bytes | Select-Object -Skip $headerLen) -join '')
}

function Export-Json {
    param([string] $Pcap, [string] $JsonPath)
    Write-Information "Exporting packet dissections as JSON -> $JsonPath (this can take a while)"
    & $TShark -r $Pcap -T json 2>$null | Out-File -FilePath $JsonPath -Encoding utf8
    $mb = [math]::Round((Get-Item $JsonPath).Length / 1MB, 1)
    Write-Information "  $mb MB"
    if ($mb -gt 90) {
        Write-Warning "$JsonPath is larger than GitHub's 100 MB file limit allows comfortably; compressing to .zip and removing the .json."
        Compress-Archive -Path $JsonPath -DestinationPath "$JsonPath.zip" -Force
        Remove-Item $JsonPath
    }
}

function Write-PluginArtifacts {
    param([string] $Pcap, [string] $Pcapng)
    $dev = Find-ControllerAddress -Pcap $Pcap
    if (-not $dev) { throw "No device descriptor with idVendor 0x$VendorId in $Pcap." }
    $addr = $dev.Address
    Write-Information "Controller: address $addr, idProduct $($dev.ProductId), bcdDevice $($dev.BcdDevice)"

    Invoke-TShark @('-r', $Pcap, '-Y', "usb.device_address == $addr", '-w', $Pcapng, '-F', 'pcapng') | Out-Null
    Write-Information "Wrote $Pcapng (controller frames only)"
    # Keep the full raw capture too, in case the hub-level context matters.
    Copy-Item $Pcap (Join-Path $OutputDir 'sc26-plugin-allhub.pcap') -Force

    $summary = New-Object System.Collections.Generic.List[string]
    $summary.Add("Steam Controller plug-in capture, $(Get-Date -Format s)")
    $summary.Add("idVendor      : 0x$VendorId")
    $summary.Add("idProduct     : $($dev.ProductId)")
    $summary.Add("bcdDevice     : $($dev.BcdDevice)   (sc26_usb::version)")
    $summary.Add("USB address   : $addr")
    $cfg = Invoke-TShark @('-r', $Pcap, '-Y', "usb.device_address == $addr && usb.bNumInterfaces", '-T', 'fields',
        '-e', 'usb.bNumInterfaces', '-e', 'usb.bInterfaceNumber', '-e', 'usb.bInterfaceClass', '-e', 'usb.bInterfaceSubClass', '-e', 'usb.bInterfaceProtocol',
        '-e', 'usbhid.descriptor.hid.wDescriptorLength', '-E', 'separator=|', '-E', 'occurrence=a')
    foreach ($row in @($cfg)) {
        $f = "$row" -split '\|'
        if ($f.Count -ge 5) {
            $summary.Add("bNumInterfaces: $($f[0])")
            $summary.Add("interfaces    : numbers=$($f[1]) classes=$($f[2]) subclasses=$($f[3]) protocols=$($f[4]) hidReportLengths=$($f[5])")
        }
    }

    # HID report descriptors: GET DESCRIPTOR (HID Report) responses for this device.
    $frames = Invoke-TShark @('-r', $Pcap, '-Y', "usb.device_address == $addr && usb.transfer_type == 0x02 && usbhid.item.bSize", '-T', 'fields',
        '-e', 'frame.number', '-e', 'usb.setup.wIndex', '-e', 'usb.control.Response', '-E', 'separator=|')
    $n = 0
    $seen = @{}
    foreach ($row in @($frames)) {
        $f = "$row" -split '\|'
        if (-not $f[0]) { continue }
        $hex = if ($f.Count -ge 3) { $f[2] -replace '[^0-9a-fA-F]', '' } else { '' }
        if (-not $hex) { $hex = Get-FramePayloadHex -Pcap $Pcap -FrameNumber ([int] $f[0]) }
        if (-not $hex -or $seen.ContainsKey($hex)) { continue }
        $seen[$hex] = $true
        $n++
        $path = Join-Path $OutputDir "sc26-report-descriptor-$n.hex"
        $hex.ToLowerInvariant() | Set-Content -Path $path -Encoding ascii -NoNewline
        $summary.Add("report desc $n : frame $($f[0]), wIndex(interface)=$($f[1]), $($hex.Length / 2) bytes -> $(Split-Path -Leaf $path)")
        Write-Information "Report descriptor $n ($($hex.Length / 2) bytes) -> $path"
    }
    if ($n -eq 0) { Write-Warning "No HID report descriptor frames found. Was the controller plugged in *after* the capture started?" }

    $summary | Set-Content -Path (Join-Path $OutputDir 'sc26-plugin-summary.txt') -Encoding utf8
    $summary | ForEach-Object { Write-Information "  $_" }
}

function Write-SteamArtifacts {
    param([string] $Pcap, [string] $Pcapng)
    $dev = Find-ControllerAddress -Pcap $Pcap
    $filter = if ($dev) { "usb.device_address == $($dev.Address)" } else { Write-Warning 'No injected device descriptor found; keeping every frame on the hub.'; 'usb' }
    Invoke-TShark @('-r', $Pcap, '-Y', $filter, '-w', $Pcapng, '-F', 'pcapng') | Out-Null
    Write-Information "Wrote $Pcapng"

    $control = Join-Path $OutputDir 'sc26-steam-control.tsv'
    "frame`ttime`tdir`tbRequest`twValue`twIndex`twLength`tsetup_report_type`tsetup_report_id`tpayload" | Set-Content $control -Encoding utf8
    Invoke-TShark @('-r', $Pcapng, '-Y', 'usb.transfer_type == 0x02', '-T', 'fields',
        '-e', 'frame.number', '-e', 'frame.time_relative', '-e', 'usb.irp_info.direction', '-e', 'usb.setup.bRequest', '-e', 'usb.setup.wValue', '-e', 'usb.setup.wIndex', '-e', 'usb.setup.wLength',
        '-e', 'usbhid.setup.ReportType', '-e', 'usbhid.setup.ReportID', '-e', 'usb.data_fragment', '-E', 'separator=/t') | Add-Content $control -Encoding utf8

    $interrupt = Join-Path $OutputDir 'sc26-steam-interrupt.tsv'
    "frame`ttime`tendpoint`tdir`tpayload" | Set-Content $interrupt -Encoding utf8
    Invoke-TShark @('-r', $Pcapng, '-Y', 'usb.transfer_type == 0x01 && usb.capdata', '-T', 'fields',
        '-e', 'frame.number', '-e', 'frame.time_relative', '-e', 'usb.endpoint_address', '-e', 'usb.irp_info.direction', '-e', 'usb.capdata', '-E', 'separator=/t') | Add-Content $interrupt -Encoding utf8

    $summary = New-Object System.Collections.Generic.List[string]
    $summary.Add("Steam handshake/traffic capture, $(Get-Date -Format s)")
    $ctrlRows = Get-Content $control | Select-Object -Skip 1
    $summary.Add("control transfers : $($ctrlRows.Count)  (SET_REPORT bRequest=0x09, GET_REPORT bRequest=0x01)")
    $byReq = $ctrlRows | ForEach-Object { ($_ -split "`t")[3] } | Where-Object { $_ } | Group-Object | Sort-Object Name
    foreach ($g in $byReq) { $summary.Add("  bRequest $($g.Name): $($g.Count)") }
    $intRows = Get-Content $interrupt | Select-Object -Skip 1
    $summary.Add("interrupt transfers: $($intRows.Count)")
    $byEpId = $intRows | ForEach-Object { $f = $_ -split "`t"; "$($f[2]) dir=$($f[3]) report_id=0x$($f[4].Substring(0, [math]::Min(2, $f[4].Length)))" } | Group-Object | Sort-Object Name
    foreach ($g in $byEpId) { $summary.Add("  $($g.Name): $($g.Count)") }
    $summary | Set-Content -Path (Join-Path $OutputDir 'sc26-steam-summary.txt') -Encoding utf8
    $summary | ForEach-Object { Write-Information "  $_" }
}

# ------------------------------------------------------------------------------------------------
if (-not (Test-IsElevated)) { throw 'Run this from an elevated PowerShell: USBPcapCMD needs administrator rights.' }
foreach ($exe in $UsbPcapCmd, $TShark) { if (-not (Test-Path $exe)) { throw "Missing $exe" } }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path
$interfaces = Get-UsbPcapInterfaces
if ($interfaces.Count -eq 0) { throw 'No \\.\USBPcapN control devices exist. Reboot after installing USBPcap (the root-hub filter attaches at boot), then retry.' }
Write-Information "USBPcap root hubs: $($interfaces -join ', ')"
$interfaceFile = Join-Path $OutputDir 'usbpcap-interface.txt'
$tmp = Join-Path $env:TEMP "sc26-capture-$PID"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

switch ($Phase) {
    'Plugin' {
        if (Test-ControllerPresent) {
            Write-Warning "A VID_$VendorId device is already connected. Unplug it now; the descriptors are only requested at plug-in."
            while (Test-ControllerPresent) { Start-Sleep -Seconds 1 }
            Write-Information 'Unplugged.'
        }
        $procs = @{}
        foreach ($i in $interfaces) {
            $file = Join-Path $tmp (($i -replace '[^A-Za-z0-9]', '') + '.pcap')
            $procs[$i] = @{ Process = (Start-UsbCapture -Interface $i -OutFile $file); File = $file }
        }
        Write-Host ''
        Write-Host '>>> Capturing on all root hubs. PLUG THE CONTROLLER IN over USB-C now. <<<' -ForegroundColor Yellow
        $deadline = (Get-Date).AddMinutes(5)
        while (-not (Test-ControllerPresent)) {
            if ((Get-Date) -gt $deadline) { foreach ($p in $procs.Values) { Stop-ConsoleProcess $p.Process }; throw 'No controller appeared within 5 minutes.' }
            Start-Sleep -Milliseconds 500
        }
        Write-Information "Controller enumerated; recording $SettleSeconds more seconds."
        Start-Sleep -Seconds $SettleSeconds
        foreach ($p in $procs.Values) { Stop-ConsoleProcess $p.Process }

        $chosen = $null
        foreach ($i in $interfaces) {
            $file = $procs[$i].File
            if ((Get-Item $file -ErrorAction SilentlyContinue).Length -gt 24 -and (Find-ControllerAddress -Pcap $file)) { $chosen = $i; break }
        }
        if (-not $chosen) { throw "None of the hub captures in $tmp contains a VID_$VendorId device descriptor. Check the cable/port and retry." }
        Write-Information "Controller is on $chosen"
        $chosen | Set-Content $interfaceFile -Encoding ascii -NoNewline

        Write-PluginArtifacts -Pcap $procs[$chosen].File -Pcapng (Join-Path $OutputDir 'sc26-plugin.pcapng')
        Export-Json -Pcap (Join-Path $OutputDir 'sc26-plugin.pcapng') -JsonPath (Join-Path $OutputDir 'sc26-plugin.json')

        $hidScript = Join-Path $PSScriptRoot 'Get-SteamControllerHid.ps1'
        & $hidScript -VendorId $VendorId | Tee-Object -FilePath (Join-Path $OutputDir 'steam-controller-hid.txt') | Out-Null
        Write-Information "Wrote $(Join-Path $OutputDir 'steam-controller-hid.txt')"
    }
    'Steam' {
        if (-not (Test-ControllerPresent)) { throw "No VID_$VendorId device connected. Plug the controller in over USB-C first." }
        while (Get-Process -Name steam -ErrorAction SilentlyContinue) {
            Write-Host 'Steam is running. Quit it completely (Steam -> Exit), waiting...' -ForegroundColor Yellow
            Start-Sleep -Seconds 3
        }
        $iface = if (Test-Path $interfaceFile) { (Get-Content $interfaceFile -Raw).Trim() } else { $null }
        if (-not $iface -or $interfaces -notcontains $iface) {
            Write-Warning 'No usable hub from the Plugin phase; capturing every hub and picking the one with the injected controller descriptor.'
            $candidates = $interfaces
        } else { $candidates = @($iface) }
        $procs = @{}
        foreach ($i in $candidates) {
            $file = Join-Path $tmp (($i -replace '[^A-Za-z0-9]', '') + '-steam.pcap')
            $procs[$i] = @{ Process = (Start-UsbCapture -Interface $i -OutFile $file -InjectDescriptors); File = $file }
        }
        Write-Host ''
        Write-Host '>>> Capturing. Now: start Steam, Settings -> Controller (DECLINE any firmware update and note it),' -ForegroundColor Yellow
        Write-Host '    open Big Picture, press every button, touch both pads, move both sticks, pull triggers, click grips,' -ForegroundColor Yellow
        Write-Host '    trigger rumble, then UNPLUG the controller. Press Enter here when done. <<<' -ForegroundColor Yellow
        Read-Host | Out-Null
        foreach ($p in $procs.Values) { Stop-ConsoleProcess $p.Process }

        $chosen = $null
        foreach ($i in $candidates) {
            $file = $procs[$i].File
            if ((Get-Item $file -ErrorAction SilentlyContinue).Length -gt 24 -and ($candidates.Count -eq 1 -or (Find-ControllerAddress -Pcap $file))) { $chosen = $i; break }
        }
        if (-not $chosen) { throw "No capture in $tmp contains the controller." }
        Write-SteamArtifacts -Pcap $procs[$chosen].File -Pcapng (Join-Path $OutputDir 'sc26-steam.pcapng')
        Export-Json -Pcap (Join-Path $OutputDir 'sc26-steam.pcapng') -JsonPath (Join-Path $OutputDir 'sc26-steam.json')
    }
}

Write-Host ''
Write-Host "Done. Raw hub captures are in $tmp. Review $OutputDir, then: git add captures; git commit -m 'captures: Steam Controller (2026) $($Phase.ToLower()) capture'" -ForegroundColor Green
