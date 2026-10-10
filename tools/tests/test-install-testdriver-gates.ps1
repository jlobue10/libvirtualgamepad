[CmdletBinding()]
param()

# Install-TestDriver.ps1's install and probe steps against a shim setup tool: the INF path
# reaches the tool as one argument even under a directory with a space (Start-Process
# -ArgumentList used to split it, so the tool printed its usage and the script carried on),
# and a failing install or probe fails the script instead of logging and exiting 0.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot '..\test-rig\Install-TestDriver.ps1'
$text = Get-Content -LiteralPath $script -Raw
$start = $text.IndexOf("Log 'installing the driver'")
$end = $text.IndexOf("Log 'done'")
if ($start -lt 0 -or $end -le $start) { throw 'install/probe section not found in Install-TestDriver.ps1' }
$section = $text.Substring($start, $end - $start)

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("install-gates " + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $shim = Join-Path $work 'setup.ps1'
    # The shim records its arguments (one per line) and exits with the code in SETUP_EXIT.
    Set-Content -LiteralPath $shim -Value @'
if ($args.Count -gt 0 -and $args[0] -eq 'install') { $args | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'setup-args.txt') }
exit [int]$env:SETUP_EXIT
'@
    $probe = Join-Path $work 'probe.ps1'
    Set-Content -LiteralPath $probe -Value 'exit [int]$env:PROBE_EXIT'
    $inf = Join-Path $work 'driver\VibeshineVhfGamepad.inf'
    New-Item -ItemType Directory -Path (Split-Path $inf) | Out-Null
    Set-Content -LiteralPath $inf -Value ';'

    $harness = Join-Path $work 'harness.ps1'
    $prefix = @"
`$ErrorActionPreference = 'Continue'
`$LogPath = '$(Join-Path $work 'install.log')'
function Log(`$m) { `$m | Out-File -FilePath `$LogPath -Append }
function Get-CimInstance { param(`$ClassName) @() }
`$setup = '$shim'
`$inf = '$inf'
`$ProbePath = '$probe'
`$Probe = [bool]::Parse('__PROBE__')
"@
    Set-Content -LiteralPath $harness -Value ($prefix + "`n" + $section + "`nLog 'done'`n")

    function Invoke-Harness {
        param([int] $SetupExit, [int] $ProbeExit, [bool] $Probe)
        $env:SETUP_EXIT = $SetupExit
        $env:PROBE_EXIT = $ProbeExit
        $text = (Get-Content -LiteralPath $harness -Raw).Replace('__PROBE__', $Probe.ToString())
        $run = Join-Path $work 'run.ps1'
        Set-Content -LiteralPath $run -Value $text
        Remove-Item -LiteralPath (Join-Path $work 'setup-args.txt') -ErrorAction SilentlyContinue
        $p = Start-Process -FilePath 'pwsh' -ArgumentList @('-NoProfile', '-NonInteractive', '-File', ('"{0}"' -f $run)) -Wait -PassThru -NoNewWindow
        return $p.ExitCode
    }

    $failures = 0
    function Check([bool] $ok, [string] $what) {
        if ($ok) { "PASS $what" } else { "FAIL $what"; $script:failures++ }
    }

    $code = Invoke-Harness -SetupExit 0 -ProbeExit 0 -Probe $true
    $argsSeen = @(Get-Content -LiteralPath (Join-Path $work 'setup-args.txt'))
    Check ($argsSeen.Count -eq 3 -and $argsSeen[0] -eq 'install' -and $argsSeen[1] -eq '--inf' -and $argsSeen[2] -eq $inf) `
        "the setup tool receives install --inf <inf> as three arguments under a path with a space (got: $($argsSeen -join ' | '))"
    Check ($code -eq 0) "a clean install and probe exit 0 (got $code)"

    $code = Invoke-Harness -SetupExit 1 -ProbeExit 0 -Probe $true
    Check ($code -ne 0 -and $code -ne 3010) "a failed install fails the script (got $code)"

    $code = Invoke-Harness -SetupExit 3010 -ProbeExit 0 -Probe $true
    Check ($code -eq 3010) "a reboot-required install exits 3010 (got $code)"

    $code = Invoke-Harness -SetupExit 0 -ProbeExit 2 -Probe $true
    Check ($code -ne 0) "a failed probe fails the script (got $code)"

    $code = Invoke-Harness -SetupExit 0 -ProbeExit 2 -Probe $false
    Check ($code -eq 0) "without -Probe the probe result is not consulted (got $code)"

    if ($failures -ne 0) { throw "$failures install gate check(s) failed" }
    'install gates: all checks passed'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
