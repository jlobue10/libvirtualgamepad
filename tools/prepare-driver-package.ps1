[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'ARM64')]
    [string] $Platform,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d{2}/\d{2}/\d{4},\d+\.\d+\.\d+\.\d+$')]
    [string] $DriverVer,

    [Parameter(Mandatory = $true)]
    [string] $OutputDir,

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [string] $InfVerifPath,
    [string] $Inf2CatPath,
    [string] $CatalogGenerationEvidencePath,

    # InfVerif ships with the WDK, not the SDK, so a host can carry every other
    # packaging tool without it. Skipping is explicit and loud because it drops
    # a real INF quality gate.
    [switch] $SkipInfVerif,

    [ValidateSet('Release', 'LocalTest')]
    [string] $SigningMode = 'Release',

    [string] $SignToolPath,

    [ValidatePattern('^[0-9a-fA-F ]{40,64}$')]
    [string] $SigningThumbprint
)

$ErrorActionPreference = 'Stop'

function Resolve-SdkTool {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,
        [string] $ExplicitPath,
        [string[]] $AdditionalSearchRoots = @()
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "$Name was not found at '$ExplicitPath'."
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $sdkRoots = @($AdditionalSearchRoots) + @(
        (Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)) 'Windows Kits\10\bin')
    )
    foreach ($sdkRoot in $sdkRoots) {
        if (-not (Test-Path -LiteralPath $sdkRoot -PathType Container)) {
            continue
        }

        foreach ($architecture in @('x64', 'x86')) {
            $directCandidate = Join-Path $sdkRoot "$architecture/$Name"
            if (Test-Path -LiteralPath $directCandidate -PathType Leaf) {
                return (Resolve-Path -LiteralPath $directCandidate).Path
            }
        }

        $versionDirectories = @(Get-ChildItem -LiteralPath $sdkRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
        foreach ($versionDirectory in $versionDirectories) {
            foreach ($architecture in @('x64', 'x86')) {
                $candidate = Join-Path $versionDirectory.FullName "$architecture/$Name"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    return (Resolve-Path -LiteralPath $candidate).Path
                }
            }
        }
    }

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "$Name was not found. Install a matching Windows SDK/WDK or pass -$($Name -replace '\.exe$','')Path."
    }
    return $command.Source
}

function Test-CodeSigningCertificate {
    param(
        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.X509Certificates.X509Certificate2] $Certificate
    )

    if (-not $Certificate.HasPrivateKey -or $Certificate.NotAfter -le (Get-Date)) {
        return $false
    }

    foreach ($extension in $Certificate.Extensions) {
        if ($extension.Oid.Value -ne '2.5.29.37') {
            continue
        }
        $usage = [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($extension, $false)
        foreach ($oid in $usage.EnhancedKeyUsages) {
            if ($oid.Value -eq '1.3.6.1.5.5.7.3.3') {
                return $true
            }
        }
    }

    return $false
}

function Find-LocalTestSigningCertificate {
    param([string] $RequestedThumbprint)

    $stores = @('Cert:\CurrentUser\My', 'Cert:\LocalMachine\My')
    if (-not [string]::IsNullOrWhiteSpace($RequestedThumbprint)) {
        $normalizedThumbprint = $RequestedThumbprint.Replace(' ', '').ToUpperInvariant()
        $matchingCertificates = @(
            foreach ($store in $stores) {
                Get-ChildItem -Path $store -ErrorAction SilentlyContinue | Where-Object {
                    $_.Thumbprint.Replace(' ', '').ToUpperInvariant() -eq $normalizedThumbprint
                }
            }
        )
        $certificate = @($matchingCertificates | Select-Object -First 1)
        if ($certificate.Count -eq 0) {
            throw "No certificate with thumbprint '$RequestedThumbprint' was found in CurrentUser\\My or LocalMachine\\My."
        }
        if (-not (Test-CodeSigningCertificate -Certificate $certificate[0])) {
            throw "Certificate '$RequestedThumbprint' must be valid, have a private key, and allow Code Signing."
        }
        return $certificate[0]
    }

    $subject = 'CN=Vibeshine VHF Gamepad Test'
    $certificate = @(Get-ChildItem -Path 'Cert:\CurrentUser\My' -ErrorAction SilentlyContinue | Where-Object {
        $_.Subject -eq $subject -and (Test-CodeSigningCertificate -Certificate $_)
    } | Sort-Object NotAfter -Descending | Select-Object -First 1)
    if ($certificate.Count -ne 0) {
        return $certificate[0]
    }

    return New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -KeyUsage DigitalSignature -KeyExportPolicy Exportable -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(5)
}

function Get-CertificateStoreScope {
    param(
        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.X509Certificates.X509Certificate2] $Certificate
    )

    if ($Certificate.PSParentPath -like '*LocalMachine\My*') {
        return 'LocalMachine'
    }
    return 'CurrentUser'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$template = Join-Path $repoRoot 'driver/VibeshineVhfGamepad.inf.in'
$dll = Join-Path $repoRoot "build/$Platform/$Configuration/VibeshineVhfGamepad.dll"
$deviceSetup = Join-Path $repoRoot "build/$Platform/$Configuration/VibeshineVhfGamepadDeviceSetup.exe"
$arch = if ($Platform -eq 'x64') { 'amd64' } else { 'arm64' }
$inf2CatTarget = if ($Platform -eq 'x64') { '10_X64' } else { '10_ARM64' }

if (-not (Test-Path -LiteralPath $template -PathType Leaf)) {
    throw "INF template is missing: $template"
}
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "Driver DLL is missing: $dll. Run tools/build-driver.ps1 first."
}
if (-not (Test-Path -LiteralPath $deviceSetup -PathType Leaf)) {
    throw "Root-device setup tool is missing: $deviceSetup. Run tools/build-driver.ps1 first."
}

$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) {
    $existing = Get-ChildItem -LiteralPath $OutputDir -Force | Select-Object -First 1
    if ($null -ne $existing) {
        throw "Refusing to overwrite a non-empty package directory: $OutputDir"
    }
} else {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$driverDir = Join-Path $OutputDir 'driver'
$toolsDir = Join-Path $OutputDir 'tools'
New-Item -ItemType Directory -Path $driverDir -Force | Out-Null
New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null

$inf = Join-Path $driverDir 'VibeshineVhfGamepad.inf'
$stagedDll = Join-Path $driverDir 'VibeshineVhfGamepad.dll'
$stagedDeviceSetup = Join-Path $toolsDir 'VibeshineVhfGamepadDeviceSetup.exe'
$contents = Get-Content -LiteralPath $template -Raw
$contents = $contents.Replace('$ARCH$', $arch).Replace('08/16/2026,0.1.0.0', $DriverVer)
[System.IO.File]::WriteAllText($inf, $contents, [System.Text.Encoding]::Unicode)
Copy-Item -LiteralPath $dll -Destination $stagedDll
Copy-Item -LiteralPath $deviceSetup -Destination $stagedDeviceSetup

$inf2Cat = Resolve-SdkTool -Name 'Inf2Cat.exe' -ExplicitPath $Inf2CatPath

if ($SkipInfVerif) {
    Write-Warning 'Skipping InfVerif. Verify this INF on a host with the WDK InfVerif tool before producing a release package.'
} else {
    $wdkToolRoots = @(
        (Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)) 'Windows Kits\10\Tools')
    )
    $infVerif = Resolve-SdkTool -Name 'InfVerif.exe' -ExplicitPath $InfVerifPath -AdditionalSearchRoots $wdkToolRoots
    & $infVerif '/w' '/v' $inf
    if ($LASTEXITCODE -ne 0) {
        throw "InfVerif failed with exit code $LASTEXITCODE."
    }
}

& $inf2Cat "/driver:$driverDir" "/os:$inf2CatTarget"
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE."
}

$catalog = & (Join-Path $PSScriptRoot 'normalize-driver-catalog.ps1') -DriverDir $driverDir
if (-not (Test-Path -LiteralPath $catalog -PathType Leaf)) {
    throw "Inf2Cat completed without the expected catalog: $catalog"
}

if (-not [string]::IsNullOrWhiteSpace($CatalogGenerationEvidencePath)) {
    if ($SigningMode -ne 'Release') {
        throw '-CatalogGenerationEvidencePath is reserved for unsigned Release packages.'
    }
    $CatalogGenerationEvidencePath = [System.IO.Path]::GetFullPath($CatalogGenerationEvidencePath)
    if (Test-Path -LiteralPath $CatalogGenerationEvidencePath) {
        throw "Refusing to overwrite catalog generation evidence: $CatalogGenerationEvidencePath"
    }
    $evidenceParent = Split-Path -Parent $CatalogGenerationEvidencePath
    if (-not (Test-Path -LiteralPath $evidenceParent -PathType Container)) {
        throw "The catalog generation evidence directory does not exist: $evidenceParent"
    }
    $catalogGenerationEvidence = [ordered]@{
        schema_version = 1
        generator = 'Inf2Cat'
        package_dir = $OutputDir
        platform = $Platform
        driver_ver = $DriverVer
        files = [ordered]@{
            'driver/VibeshineVhfGamepad.inf' = (Get-FileHash -LiteralPath $inf -Algorithm SHA256).Hash.ToLowerInvariant()
            'driver/VibeshineVhfGamepad.dll' = (Get-FileHash -LiteralPath $stagedDll -Algorithm SHA256).Hash.ToLowerInvariant()
            'driver/VibeshineVhfGamepad.cat' = (Get-FileHash -LiteralPath $catalog -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $catalogGenerationEvidence | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $CatalogGenerationEvidencePath -NoNewline -Encoding utf8
}

if ($SigningMode -eq 'Release') {
    Write-Output "Prepared unsigned $Platform driver package in $OutputDir"
    Write-Output 'Next: SignPath-sign driver/VibeshineVhfGamepad.cat and tools/VibeshineVhfGamepadDeviceSetup.exe, then run tools/verify-driver-package.ps1 to verify the final package and write manifest.json.'
    return
}

$signTool = Resolve-SdkTool -Name 'signtool.exe' -ExplicitPath $SignToolPath
$certificate = Find-LocalTestSigningCertificate -RequestedThumbprint $SigningThumbprint
$certificateThumbprint = $certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
$certificatePath = Join-Path $driverDir 'VibeshineVhfGamepad.cer'
Export-Certificate -Cert $certificate -FilePath $certificatePath -Force | Out-Null

function Sign-LocalFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath
    )

    $signArguments = @('sign', '/fd', 'SHA256')
    if ((Get-CertificateStoreScope -Certificate $certificate) -eq 'LocalMachine') {
        $signArguments += '/sm'
    }
    $signArguments += @('/sha1', $certificateThumbprint, $FilePath)
    & $signTool @signArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Local signing failed for '$FilePath' with exit code $LASTEXITCODE."
    }
}

function Assert-LocalSigner {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath
    )

    $signature = Get-AuthenticodeSignature -LiteralPath $FilePath
    if ($null -eq $signature.SignerCertificate) {
        throw "Local signing completed without a signer certificate for '$FilePath'."
    }
    if ($signature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
        throw "Local signing produced a hash mismatch for '$FilePath'."
    }
    if ($signature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant() -ne $certificateThumbprint) {
        throw "The local signer for '$FilePath' does not match the exported public certificate."
    }
}

Sign-LocalFile -FilePath $catalog
# The setup tool is deliberately outside the driver catalog, so embedding its
# local signature after Inf2Cat does not change catalog membership.
Sign-LocalFile -FilePath $stagedDeviceSetup
Assert-LocalSigner -FilePath $catalog
Assert-LocalSigner -FilePath $stagedDeviceSetup

Write-Output "Prepared locally signed $Platform driver package in $OutputDir"
Write-Output "Exported the public test certificate: $certificatePath"
Write-Output 'The public certificate is not trusted automatically. On each test host, run tools/trust-test-certificate.ps1 elevated, then run tools/verify-driver-package.ps1.'
