[CmdletBinding()]
param()

# Capture-SteamController.ps1 runs an elevated Ctrl+C helper against USBPcapCMD. It must be
# passed inline (-EncodedCommand with the target PID substituted into the helper text), never
# through a file in %TEMP% that a same-user non-elevated process could replace between write
# and run (D23-01). This pins the helper's shape; the capture itself needs hardware.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot '..\capture\Capture-SteamController.ps1'
$text = Get-Content -LiteralPath $script -Raw
$failures = 0
function Check([bool] $ok, [string] $name) {
    if ($ok) { Write-Host "PASS: $name" } else { Write-Host "FAIL: $name"; $script:failures++ }
}

$helperStart = $text.IndexOf("`$script:CtrlCHelper = @'")
$helperEnd = $text.IndexOf("'@", $helperStart + 1)
Check ($helperStart -ge 0 -and $helperEnd -gt $helperStart) 'the Ctrl+C helper is a single-quoted here-string (no expansion of its text)'
$helper = $text.Substring($helperStart, $helperEnd - $helperStart)
Check ($helper -match '__TARGET_PID__') 'the helper takes its target PID through a placeholder'
Check ($helper -notmatch 'param\(') 'the helper has no parameter block (it is not run as a file)'

$fnStart = $text.IndexOf('function Stop-ConsoleProcess')
$fnEnd = $text.IndexOf('function ', $fnStart + 1)
Check ($fnStart -ge 0 -and $fnEnd -gt $fnStart) 'Stop-ConsoleProcess is present'
$fn = $text.Substring($fnStart, $fnEnd - $fnStart)
Check ($fn -match "-replace '__TARGET_PID__', \(\[string\] \`$Process\.Id\)") 'the PID is substituted as a plain string'
Check ($fn -match '-EncodedCommand') 'the helper runs from -EncodedCommand'
Check ($fn -notmatch "'-File'" -and $fn -notmatch 'Set-Content' -and $fn -notmatch 'Bypass' -and $fn -notmatch '\$env:TEMP') 'no helper file is written to a temp directory'

# The substituted helper must still be valid PowerShell with the PID in place.
$sample = ($helper -replace '__TARGET_PID__', '4242')
$sample = $sample.Substring($sample.IndexOf("@'") + 2)
$tokens = $null; $errors = $null
[void][System.Management.Automation.Language.Parser]::ParseInput($sample, [ref]$tokens, [ref]$errors)
Check ($errors.Count -eq 0 -and $sample -match "\`$TargetPid = \[int\] '4242'") 'the substituted helper parses and carries the PID'

# The raw hub captures (D25-05): random directory, never adopted, restricted ACL.
$tmpStart = $text.IndexOf('$tmp = Join-Path $env:TEMP')
$tmpBlock = if ($tmpStart -ge 0) { $text.Substring($tmpStart, [Math]::Min(600, $text.Length - $tmpStart)) } else { '' }
Check ($tmpBlock -match 'GetRandomFileName') 'the capture directory name is random'
Check ($tmpBlock -match 'New-Item -ItemType Directory -Path \$tmp' -and $tmpBlock -notmatch 'New-Item -ItemType Directory -Force -Path \$tmp') 'an existing capture directory is not adopted (no -Force)'
Check ($tmpBlock -match 'icacls \$tmp /inheritance:r') 'the capture directory ACL is restricted to Administrators and SYSTEM'

if ($failures -ne 0) { throw "$failures capture-helper gate(s) failed" }
Write-Host 'capture helper gates passed'
