[CmdletBinding()]
param()

# Collect-Evidence.ps1 copies lines of Steam's config.vdf into an evidence zip that the README
# tells the operator to commit. The selection must be anchored to the gamepad binding key: an
# unanchored, case-insensitive vendor/product id match also caught encrypted login-token hex
# lines (D29-01).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot '..\test-rig\Collect-Evidence.ps1'
$text = Get-Content -LiteralPath $script -Raw
$failures = 0
function Check([bool] $ok, [string] $name) {
    if ($ok) { Write-Host "PASS: $name" } else { Write-Host "FAIL: $name"; $script:failures++ }
}

$line = ($text -split "\r?\n" | Where-Object { $_ -match 'steam-config-gamepad-lines' } | Select-Object -First 1)
Check ($null -ne $line) 'the config.vdf extraction is present'
Check ($line -match "-Pattern '\^\\s\*""SDL_GamepadBind'") 'the config.vdf extraction is anchored to the SDL_GamepadBind key'
Check ($line -match '-CaseSensitive') 'the config.vdf extraction is case-sensitive'
Check ($line -notmatch '28de|1302') 'no bare vendor/product id pattern is applied to config.vdf'

# The anchored pattern must keep the binding lines and drop a token-like hex line.
$sample = @(
    '	"SDL_GamepadBind"		"03000000de280000020100006800000,Steam Controller,a:b0"',
    '	"ConnectCache"		"7a28de13021302ab28de"'
)
$kept = @($sample | Select-String -CaseSensitive -Pattern '^\s*"SDL_GamepadBind' | ForEach-Object { $_.Line })
Check ($kept.Count -eq 1 -and $kept[0] -match 'SDL_GamepadBind') 'the anchored pattern keeps the binding line and drops token-like hex lines'

# L44-03: the host log is reduced to the virtual-gamepad lines; the full log is never copied, and an
# absent log leaves a marker in system.txt instead of silently omitting the file.
Check ($text -notmatch 'Copy-Item\s+\$VibepolloLog') 'the full host log is never copied into the evidence zip'
Check ($text -match "Select-String -Pattern 'gamepad\|vhf\|Steam Controller\|virtual'") 'the host log is reduced to virtual-gamepad lines'
Check ($text -match 'vibepollo log: not found at \$VibepolloLog') 'an absent host log is recorded in system.txt'

if ($failures -ne 0) { throw "$failures collect-evidence gate(s) failed" }
Write-Host 'collect-evidence gates passed'
