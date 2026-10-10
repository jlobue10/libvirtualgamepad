<#
.SYNOPSIS
  Installs the test-signed Vibeshine VHF gamepad driver on a Windows 11 test machine and
  optionally runs the Steam Controller (2026) probe against it. Run from an elevated PowerShell.

.DESCRIPTION
  1. Checks Secure Boot and test signing. A test-signed driver only loads with test signing on,
     and test signing can only be turned on while Secure Boot is off (firmware setting). If the
     machine is not ready the script says exactly what to do and stops.
  2. Trusts the package's LocalTest certificate in Root and TrustedPublisher (certutil; the
     repo's trust-test-certificate.ps1 does the same with a confirmation prompt).
  3. Installs the driver with the package's own setup tool and prints its status. Exit code 3010
     from the tool means reboot, then run the script again (it is idempotent).
  4. With -Probe, runs probe_sc26_usb.exe: creates a virtual Steam Controller on the driver,
     checks everything Steam looks at, releases it.

.EXAMPLE
  .\Install-TestDriver.ps1 -PackageDir .\vhf-package -Probe
  .\Install-TestDriver.ps1 -EnableTestSigning       # only flips bcdedit; reboot afterwards
#>
[CmdletBinding()]
param(
    [string] $PackageDir = (Join-Path $PSScriptRoot 'vhf-package'),
    [string] $ProbePath = (Join-Path $PSScriptRoot 'probe_sc26_usb.exe'),
    [switch] $Probe,
    [switch] $EnableTestSigning,
    [string] $LogPath = (Join-Path $PSScriptRoot 'install-testdriver.log')
)
$ErrorActionPreference = 'Continue'
function Log($m) { "$(Get-Date -Format s) $m" | Tee-Object -FilePath $LogPath -Append | Out-Host }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal] $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from an elevated PowerShell (the driver store and bcdedit need it).'
}

$secureBoot = $null
try { $secureBoot = Confirm-SecureBootUEFI } catch { $secureBoot = "unknown ($($_.Exception.Message))" }
$testsigning = ((bcdedit /enum '{current}' 2>&1 | Select-String 'testsigning') -join '') -match 'Yes'
Log "machine: $env:COMPUTERNAME, $((Get-CimInstance Win32_OperatingSystem).Caption) build $([Environment]::OSVersion.Version.Build)"
Log "Secure Boot: $secureBoot   test signing: $testsigning"

if ($EnableTestSigning) {
    if ($secureBoot -eq $true) {
        Log 'Secure Boot is ON. Windows refuses test signing while it is on. Turn Secure Boot off in the firmware (UEFI setup), boot Windows, then run this again.'
        exit 2
    }
    bcdedit /set testsigning on | ForEach-Object { Log "  bcdedit: $_" }
    Log 'Test signing is set. Reboot, then run this script again without -EnableTestSigning.'
    exit 3010
}
if (-not $testsigning) {
    if ($secureBoot -eq $true) {
        Log 'Not ready: Secure Boot is ON and test signing is OFF. Turn Secure Boot off in the firmware, then run: .\Install-TestDriver.ps1 -EnableTestSigning, reboot, and run the install.'
    } else {
        Log 'Not ready: test signing is OFF. Run: .\Install-TestDriver.ps1 -EnableTestSigning, reboot, then run the install again.'
    }
    exit 2
}

$PackageDir = (Resolve-Path $PackageDir).Path
$cer = Join-Path $PackageDir 'driver\VibeshineVhfGamepad.cer'
$inf = Join-Path $PackageDir 'driver\VibeshineVhfGamepad.inf'
$cat = Join-Path $PackageDir 'driver\VibeshineVhfGamepad.cat'
$setup = Join-Path $PackageDir 'tools\VibeshineVhfGamepadDeviceSetup.exe'
foreach ($f in $cer, $inf, $cat, $setup) { if (-not (Test-Path $f)) { throw "missing $f" } }

# Trust the bundled certificate only after proving it signed this package's catalog and setup
# tool (the same gate as tools/trust-test-certificate.ps1); a swapped or mismatched .cer must
# never land in LocalMachine\Root.
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cer)
$thumbprint = $certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
foreach ($signed in @(@{ Path = $cat; Name = 'catalog' }, @{ Path = $setup; Name = 'setup tool' })) {
    $sig = Get-AuthenticodeSignature -LiteralPath $signed.Path
    if ($null -eq $sig.SignerCertificate) { throw "the package $($signed.Name) has no signer certificate" }
    if ($sig.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) { throw "the package $($signed.Name) has a hash mismatch" }
    if ($sig.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant() -ne $thumbprint) {
        throw "the bundled certificate ($thumbprint) did not sign the package $($signed.Name) ($($sig.SignerCertificate.Thumbprint))"
    }
    Log "$($signed.Name) signature: $($sig.Status) ($($sig.SignerCertificate.Subject))"
}
Log 'trusting the test certificate (Root + TrustedPublisher)'
certutil -addstore -f Root $cer | Select-Object -Last 1 | ForEach-Object { Log "  $_" }
certutil -addstore -f TrustedPublisher $cer | Select-Object -Last 1 | ForEach-Object { Log "  $_" }

Log 'driver status before:'
& $setup status 2>&1 | ForEach-Object { Log "  $_" }
Log 'installing the driver'
# The call operator quotes each argument (Start-Process -ArgumentList does not, so a kit under a
# path with a space made the setup tool print its usage and the script carry on to the probe).
& $setup install --inf $inf 2>&1 | ForEach-Object { Log "  $_" }
$setupExit = $LASTEXITCODE
Log "setup exit code $setupExit (0 = installed, 3010 = reboot then run again)"
Log 'driver status after:'
& $setup status 2>&1 | ForEach-Object { Log "  $_" }
$drv = Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceID -like 'ROOT\VIBESHINE*' } | Select-Object -First 1
if ($drv) { Log "installed: $($drv.InfName) $($drv.DriverVersion) $($drv.DriverDate)" }
if ($setupExit -eq 3010) { exit 3010 }
if ($setupExit -ne 0) { throw "driver install failed (setup exit code $setupExit); see $LogPath" }

if ($Probe) {
    if (-not (Test-Path $ProbePath)) { throw "missing $ProbePath" }
    Log 'running probe_sc26_usb'
    & $ProbePath 2>&1 | ForEach-Object { Log "  $_" }
    $probeExit = $LASTEXITCODE
    Log "probe exit code $probeExit (0 = PROBE PASSED)"
    if ($probeExit -ne 0) { throw "probe failed (exit code $probeExit); see $LogPath" }
}
Log 'done'
