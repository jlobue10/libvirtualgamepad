[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ReleaseDir,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v0\.1\.0-beta\.[1-9][0-9]*$')]
    [string] $Tag,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string] $Repository,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string] $SourceRevision,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $Token
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-ExactList {
    param(
        [Parameter(Mandatory = $true)][object[]] $Actual,
        [Parameter(Mandatory = $true)][object[]] $Expected,
        [Parameter(Mandatory = $true)][string] $Name
    )

    $actualText = @($Actual | ForEach-Object { [string] $_ }) -join "`n"
    $expectedText = @($Expected | ForEach-Object { [string] $_ }) -join "`n"
    if ($actualText -cne $expectedText) {
        throw "$Name does not match the release contract.`nExpected:`n$expectedText`nActual:`n$actualText"
    }
}

function Get-ResponseJson {
    param(
        [Parameter(Mandatory = $true)] $Response,
        [Parameter(Mandatory = $true)][string] $Operation
    )

    try {
        return $Response.Content | ConvertFrom-Json
    } catch {
        throw "$Operation returned invalid JSON with HTTP $($Response.StatusCode)."
    }
}

function Assert-HttpStatus {
    param(
        [Parameter(Mandatory = $true)] $Response,
        [Parameter(Mandatory = $true)][int] $ExpectedStatus,
        [Parameter(Mandatory = $true)][string] $Operation
    )

    if ([int] $Response.StatusCode -ne $ExpectedStatus) {
        $body = [string] $Response.Content
        if ($body.Length -gt 1000) {
            $body = $body.Substring(0, 1000)
        }
        throw "$Operation failed with HTTP $($Response.StatusCode): $body"
    }
}

function Invoke-GitHubJson {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Get', 'Post', 'Patch', 'Delete')][string] $Method,
        [Parameter(Mandatory = $true)][string] $Uri,
        [object] $Body
    )

    $parameters = @{
        Method = $Method
        Uri = $Uri
        Headers = $script:JsonHeaders
        SkipHttpErrorCheck = $true
        MaximumRetryCount = 0
        TimeoutSec = 120
    }
    if ($PSBoundParameters.ContainsKey('Body')) {
        $parameters.ContentType = 'application/json'
        $parameters.Body = $Body | ConvertTo-Json -Depth 8 -Compress
    }
    return Invoke-WebRequest @parameters
}

function Assert-ReleaseSet {
    param(
        [Parameter(Mandatory = $true)][string] $Root,
        [Parameter(Mandatory = $true)][string] $ExpectedTag,
        [Parameter(Mandatory = $true)][string] $ExpectedRevision,
        [Parameter(Mandatory = $true)][string[]] $ExpectedFiles,
        [Parameter(Mandatory = $true)][string] $ArchiveName,
        [Parameter(Mandatory = $true)][string] $ChecksumName,
        [Parameter(Mandatory = $true)][string] $LockName,
        [Parameter(Mandatory = $true)][string] $EvidenceName
    )

    $Root = [System.IO.Path]::GetFullPath($Root)
    $actualFiles = @(Get-ChildItem -LiteralPath $Root -File | ForEach-Object Name | Sort-Object)
    Assert-ExactList -Actual $actualFiles -Expected $ExpectedFiles -Name 'Release file set'

    $archivePath = Join-Path $Root $ArchiveName
    $checksumText = Get-Content -LiteralPath (Join-Path $Root $ChecksumName) -Raw
    $checksumMatch = [regex]::Match(
        $checksumText,
        '^([0-9a-f]{64})  ' + [regex]::Escape($ArchiveName) + '\r?\n?$'
    )
    if (-not $checksumMatch.Success) {
        throw 'The SHA-256 sidecar is malformed or names a different archive.'
    }
    $archiveHash = Get-Sha256 -Path $archivePath
    if ($archiveHash -cne $checksumMatch.Groups[1].Value) {
        throw 'The archive SHA-256 does not match its sidecar.'
    }

    $lock = Get-Content -LiteralPath (Join-Path $Root $LockName) -Raw | ConvertFrom-Json
    $evidence = Get-Content -LiteralPath (Join-Path $Root $EvidenceName) -Raw | ConvertFrom-Json
    $expectedLayout = @(
        'driver/VibeshineVhfGamepad.cat',
        'driver/VibeshineVhfGamepad.dll',
        'driver/VibeshineVhfGamepad.inf',
        'manifest.json',
        'tools/VibeshineVhfGamepadDeviceSetup.exe'
    )
    $expectedSigned = @(
        'driver/VibeshineVhfGamepad.cat',
        'tools/VibeshineVhfGamepadDeviceSetup.exe'
    )
    $expectedUnsigned = @(
        'driver/VibeshineVhfGamepad.cat',
        'driver/VibeshineVhfGamepad.dll',
        'tools/VibeshineVhfGamepadDeviceSetup.exe'
    )
    $expectedMembershipFiles = @(
        'driver/VibeshineVhfGamepad.cat',
        'driver/VibeshineVhfGamepad.dll',
        'driver/VibeshineVhfGamepad.inf'
    )

    foreach ($record in @($lock, $evidence)) {
        if ($record.schema_version -ne 1 -or
            $record.tag -cne $ExpectedTag -or
            $record.tag_target -cne $ExpectedRevision -or
            $record.source_revision -cne $ExpectedRevision -or
            $record.driver_ver -notmatch '^\d{2}/\d{2}/\d{4},0\.1\.0\.\d+$' -or
            $record.protocol_version -lt 1 -or
            $record.platform -cne 'x64' -or
            $record.archive.name -cne $ArchiveName -or
            $record.archive.sha256 -cne $archiveHash -or
            $record.manifest.path -cne 'manifest.json' -or
            $record.manifest.sha256 -notmatch '^[0-9a-f]{64}$' -or
            $record.signing.channel -cne 'msi-request-signing' -or
            (@($record.layout) -join "`n") -cne ($expectedLayout -join "`n") -or
            (@($record.signing.signed_downstream) -join "`n") -cne ($expectedSigned -join "`n") -or
            (@($record.signing.unsigned_payloads) -join "`n") -cne ($expectedUnsigned -join "`n") -or
            $record.signing.catalog_membership.basis -cne 'fresh-inf2cat' -or
            $record.signing.catalog_membership.generator -cne 'Inf2Cat' -or
            (@($record.signing.catalog_membership.files.PSObject.Properties.Name | Sort-Object) -join "`n") -cne
                ($expectedMembershipFiles -join "`n")) {
            throw 'A release lock or evidence record does not match the release contract.'
        }
    }
    if ($lock.driver_ver -cne $evidence.driver_ver -or
        $lock.protocol_version -ne $evidence.protocol_version -or
        $lock.manifest.sha256 -cne $evidence.manifest.sha256 -or
        ($lock.signing.catalog_membership | ConvertTo-Json -Depth 4 -Compress) -cne
            ($evidence.signing.catalog_membership | ConvertTo-Json -Depth 4 -Compress)) {
        throw 'The release lock and evidence record disagree.'
    }
    if (-not $evidence.verification.clean_tagged_head -or
        -not $evidence.verification.infverif_required -or
        -not $evidence.verification.inf2cat_required -or
        -not $evidence.verification.fresh_inf2cat_evidence -or
        -not $evidence.verification.package_verified_unsigned_for_msi_signing -or
        -not $evidence.verification.unsigned_authenticode_checked -or
        -not $evidence.verification.certificate_absent -or
        -not $evidence.verification.archive_created_once) {
        throw 'The evidence record does not attest every producer release gate.'
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $zipLayout = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object)
        Assert-ExactList -Actual $zipLayout -Expected $expectedLayout -Name 'ZIP layout'
        $manifestEntry = $zip.GetEntry('manifest.json')
        if ($null -eq $manifestEntry) {
            throw 'The ZIP has no manifest.json entry.'
        }
        $stream = $manifestEntry.Open()
        $memory = [System.IO.MemoryStream]::new()
        try {
            $stream.CopyTo($memory)
            $manifestBytes = $memory.ToArray()
        } finally {
            $memory.Dispose()
            $stream.Dispose()
        }
    } finally {
        $zip.Dispose()
    }

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $manifestHash = ([System.BitConverter]::ToString(
            $sha256.ComputeHash($manifestBytes)
        )).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
    if ($manifestHash -cne $lock.manifest.sha256) {
        throw 'The archived manifest hash does not match the release lock.'
    }
    $manifest = [System.Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
    if ($manifest.source_revision -cne $lock.source_revision -or
        $manifest.driver_ver -cne $lock.driver_ver -or
        $manifest.protocol_version -ne $lock.protocol_version -or
        $manifest.platform -cne $lock.platform -or
        $manifest.signing.channel -cne 'msi-request-signing' -or
        (@($manifest.signing.signed_downstream) -join "`n") -cne ($expectedSigned -join "`n") -or
        (@($manifest.signing.unsigned_payloads) -join "`n") -cne ($expectedUnsigned -join "`n") -or
        ($manifest.signing.catalog_membership | ConvertTo-Json -Depth 4 -Compress) -cne
            ($lock.signing.catalog_membership | ConvertTo-Json -Depth 4 -Compress)) {
        throw 'The archived manifest does not agree with the release lock.'
    }

    $hashes = [ordered]@{}
    $sizes = [ordered]@{}
    foreach ($name in $ExpectedFiles) {
        $path = Join-Path $Root $name
        $hashes[$name] = Get-Sha256 -Path $path
        $sizes[$name] = (Get-Item -LiteralPath $path).Length
    }
    return [pscustomobject]@{
        Hashes = $hashes
        Sizes = $sizes
    }
}

function Assert-DirectoryHashesEqual {
    param(
        [Parameter(Mandatory = $true)][string] $ExpectedRoot,
        [Parameter(Mandatory = $true)][string] $ActualRoot,
        [Parameter(Mandatory = $true)][string[]] $Names
    )

    foreach ($name in $Names) {
        $expectedHash = Get-Sha256 -Path (Join-Path $ExpectedRoot $name)
        $actualHash = Get-Sha256 -Path (Join-Path $ActualRoot $name)
        if ($actualHash -cne $expectedHash) {
            throw "Downloaded GitHub asset '$name' does not match the internal artifact bytes."
        }
    }
}

function Get-ReleaseAssets {
    param([Parameter(Mandatory = $true)][long] $ReleaseId)

    $uri = "$script:ApiBase/releases/$ReleaseId/assets?per_page=100"
    $response = Invoke-GitHubJson -Method Get -Uri $uri
    Assert-HttpStatus -Response $response -ExpectedStatus 200 -Operation "List assets for release $ReleaseId"
    return @(Get-ResponseJson -Response $response -Operation "List assets for release $ReleaseId")
}

function Assert-AssetSet {
    param(
        [Parameter(Mandatory = $true)][object[]] $Assets,
        [Parameter(Mandatory = $true)][string[]] $ExpectedNames,
        [Parameter(Mandatory = $true)] $ExpectedSizes,
        [System.Collections.IDictionary] $ExpectedIds
    )

    if ($Assets.Count -ne $ExpectedNames.Count) {
        throw "GitHub release has $($Assets.Count) assets; expected exactly $($ExpectedNames.Count)."
    }
    $names = @($Assets | ForEach-Object { [string] $_.name } | Sort-Object)
    Assert-ExactList -Actual $names -Expected $ExpectedNames -Name 'GitHub asset names'
    if (@($names | Sort-Object -Unique).Count -ne $ExpectedNames.Count) {
        throw 'GitHub release asset names are not unique.'
    }

    $byName = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal
    )
    foreach ($asset in $Assets) {
        $name = [string] $asset.name
        if ($byName.ContainsKey($name)) {
            throw "GitHub returned duplicate asset name '$name'."
        }
        $byName.Add($name, $asset)
        if ([string] $asset.state -cne 'uploaded' -or
            [string] $asset.content_type -cne 'application/octet-stream') {
            throw "GitHub asset '$name' is not a completed application/octet-stream upload."
        }
        if ([long] $asset.size -ne [long] $ExpectedSizes[$name]) {
            throw "GitHub asset '$name' has size '$($asset.size)', expected '$($ExpectedSizes[$name])'."
        }
        $assetId = 0L
        if (-not [long]::TryParse([string] $asset.id, [ref] $assetId) -or $assetId -le 0) {
            throw "GitHub asset '$name' has invalid ID '$($asset.id)'."
        }
        if ($null -ne $ExpectedIds -and
            ([long] $ExpectedIds[$name] -ne $assetId)) {
            throw "GitHub asset '$name' changed ID from '$($ExpectedIds[$name])' to '$assetId'."
        }
    }
    return ,$byName
}

function Save-ReleaseAssets {
    param(
        [Parameter(Mandatory = $true)] $AssetsByName,
        [Parameter(Mandatory = $true)][string[]] $ExpectedNames,
        [Parameter(Mandatory = $true)][string] $Destination
    )

    if (Test-Path -LiteralPath $Destination) {
        throw "Refusing to reuse asset download directory '$Destination'."
    }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    foreach ($name in $ExpectedNames) {
        $asset = $AssetsByName[$name]
        $destinationPath = Join-Path $Destination $name
        $response = Invoke-WebRequest `
            -Method Get `
            -Uri "$script:ApiBase/releases/assets/$($asset.id)" `
            -Headers $script:BinaryHeaders `
            -OutFile $destinationPath `
            -TimeoutSec 300 `
            -PassThru `
            -SkipHttpErrorCheck `
            -MaximumRetryCount 0
        Assert-HttpStatus -Response $response -ExpectedStatus 200 -Operation "Download asset '$name'"
        if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
            throw "GitHub did not write downloaded asset '$name'."
        }
    }
}

function Assert-ReleaseObject {
    param(
        [Parameter(Mandatory = $true)] $Release,
        [Parameter(Mandatory = $true)][long] $ExpectedId,
        [Parameter(Mandatory = $true)][bool] $ExpectedDraft,
        [Parameter(Mandatory = $true)][string] $ExpectedTag,
        [Parameter(Mandatory = $true)][string] $ExpectedBody,
        [switch] $RequireImmutable
    )

    if ($Release.draft -isnot [bool] -or $Release.prerelease -isnot [bool]) {
        throw "GitHub release $ExpectedId returned non-boolean draft or prerelease state."
    }
    if ([long] $Release.id -ne $ExpectedId -or
        [string] $Release.tag_name -cne $ExpectedTag -or
        [string] $Release.name -cne $ExpectedTag -or
        [string] $Release.body -cne $ExpectedBody -or
        $Release.draft -ne $ExpectedDraft -or
        $Release.prerelease -ne $true) {
        throw "GitHub release $ExpectedId does not match the captured tag, title, body, draft, or prerelease state."
    }
    if ($RequireImmutable -and
        ($Release.immutable -isnot [bool] -or $Release.immutable -ne $true)) {
        throw "GitHub release $ExpectedId is published but is not reported as immutable."
    }
}

function Assert-LightweightTagTarget {
    param(
        [Parameter(Mandatory = $true)][string] $ExpectedTag,
        [Parameter(Mandatory = $true)][string] $ExpectedRevision
    )

    $encodedRefTag = [System.Uri]::EscapeDataString($ExpectedTag)
    $response = Invoke-GitHubJson -Method Get -Uri "$script:ApiBase/git/ref/tags/$encodedRefTag"
    Assert-HttpStatus -Response $response -ExpectedStatus 200 -Operation "Resolve tag '$ExpectedTag'"
    $tagRef = Get-ResponseJson -Response $response -Operation "Resolve tag '$ExpectedTag'"
    if ([string] $tagRef.ref -cne "refs/tags/$ExpectedTag" -or
        [string] $tagRef.object.type -cne 'commit' -or
        [string] $tagRef.object.sha -cne $ExpectedRevision) {
        throw "Tag '$ExpectedTag' is not a lightweight reference to exact commit '$ExpectedRevision'."
    }
}

$ReleaseDir = [System.IO.Path]::GetFullPath($ReleaseDir)
$SourceRevision = $SourceRevision.ToLowerInvariant()
$version = $Tag.Substring(1)
$assetBase = "libvirtualgamepad-$version-windows-x64"
$archiveName = "$assetBase.zip"
$checksumName = "$archiveName.sha256"
$lockName = "$assetBase.release-lock.json"
$evidenceName = "$assetBase-evidence.json"
$expectedFiles = @($archiveName, $checksumName, $evidenceName, $lockName) | Sort-Object
$releaseBody = 'Unsigned Windows x64 producer package for downstream SignPath signing.' +
    "`n`n" +
    'Consumers must verify the SHA-256 and release lock, then sign only the catalog and setup tool in their MSI signing request.'

$script:ApiBase = "https://api.github.com/repos/$Repository"
$script:JsonHeaders = @{
    Authorization = "Bearer $Token"
    Accept = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2026-03-10'
}
$script:BinaryHeaders = @{
    Authorization = "Bearer $Token"
    Accept = 'application/octet-stream'
    'X-GitHub-Api-Version' = '2026-03-10'
}

$internal = Assert-ReleaseSet `
    -Root $ReleaseDir `
    -ExpectedTag $Tag `
    -ExpectedRevision $SourceRevision `
    -ExpectedFiles $expectedFiles `
    -ArchiveName $archiveName `
    -ChecksumName $checksumName `
    -LockName $lockName `
    -EvidenceName $evidenceName

# The preflight is intentionally advisory to the transaction boundary: it gives
# a clear early error, while the create-only POST below is what closes the race.
$encodedTag = [System.Uri]::EscapeDataString($Tag)
$tagResponse = Invoke-GitHubJson -Method Get -Uri "$script:ApiBase/releases/tags/$encodedTag"
if ([int] $tagResponse.StatusCode -eq 200) {
    throw "A GitHub release already exists for '$Tag'."
}
if ([int] $tagResponse.StatusCode -ne 404) {
    Assert-HttpStatus -Response $tagResponse -ExpectedStatus 404 -Operation "Preflight release '$Tag'"
}

$page = 1
do {
    $listResponse = Invoke-GitHubJson -Method Get -Uri "$script:ApiBase/releases?per_page=100&page=$page"
    Assert-HttpStatus -Response $listResponse -ExpectedStatus 200 -Operation "Preflight release page $page"
    $releases = @(Get-ResponseJson -Response $listResponse -Operation "Preflight release page $page")
    $existingAssets = @($releases | ForEach-Object { $_.assets } | ForEach-Object { [string] $_.name })
    $duplicates = @($expectedFiles | Where-Object { $_ -cin $existingAssets })
    if ($duplicates.Count -ne 0) {
        throw "A release asset already exists with a protected name: $($duplicates -join ', ')."
    }
    ++$page
} while ($releases.Count -eq 100)

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("libvirtualgamepad-publish-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
$createdDraftReleaseId = $null
$publicationAttempted = $false
$transactionComplete = $false
$createPayload = [ordered]@{
    tag_name = $Tag
    target_commitish = $SourceRevision
    name = $Tag
    body = $releaseBody
    draft = [bool] $true
    prerelease = [bool] $true
}

try {
    Assert-LightweightTagTarget -ExpectedTag $Tag -ExpectedRevision $SourceRevision
    $createResponse = Invoke-GitHubJson -Method Post -Uri "$script:ApiBase/releases" -Body $createPayload
    Assert-HttpStatus -Response $createResponse -ExpectedStatus 201 -Operation "Create draft release '$Tag'"
    $created = Get-ResponseJson -Response $createResponse -Operation "Create draft release '$Tag'"
    $parsedReleaseId = 0L
    if (-not [long]::TryParse([string] $created.id, [ref] $parsedReleaseId) -or $parsedReleaseId -le 0) {
        throw "The newly created draft returned invalid release ID '$($created.id)'."
    }
    $createdDraftReleaseId = $parsedReleaseId
    Assert-ReleaseObject `
        -Release $created `
        -ExpectedId $createdDraftReleaseId `
        -ExpectedDraft $true `
        -ExpectedTag $Tag `
        -ExpectedBody $releaseBody
    if (@($created.assets).Count -ne 0) {
        throw "The newly created draft release $createdDraftReleaseId already contains assets."
    }

    $uploadTemplate = [string] $created.upload_url
    $uploadBaseText = $uploadTemplate -replace '\{\?name,label\}$', ''
    $uploadBase = [System.Uri] $uploadBaseText
    $expectedUploadPath = "/repos/$Repository/releases/$createdDraftReleaseId/assets"
    if ($uploadBase.Scheme -cne 'https' -or
        $uploadBase.Host -cne 'uploads.github.com' -or
        $uploadBase.AbsolutePath -cne $expectedUploadPath) {
        throw "The newly created draft returned an unexpected upload URL '$uploadTemplate'."
    }

    $uploadedIds = [ordered]@{}
    foreach ($name in $expectedFiles) {
        $path = Join-Path $ReleaseDir $name
        $encodedName = [System.Uri]::EscapeDataString($name)
        $uploadResponse = Invoke-WebRequest `
            -Method Post `
            -Uri "${uploadBaseText}?name=$encodedName" `
            -Headers $script:JsonHeaders `
            -ContentType 'application/octet-stream' `
            -TimeoutSec 300 `
            -InFile $path `
            -SkipHttpErrorCheck `
            -MaximumRetryCount 0
        Assert-HttpStatus -Response $uploadResponse -ExpectedStatus 201 -Operation "Upload asset '$name'"
        $uploaded = Get-ResponseJson -Response $uploadResponse -Operation "Upload asset '$name'"
        $uploadedId = 0L
        if ([string] $uploaded.name -cne $name -or
            [long] $uploaded.size -ne [long] $internal.Sizes[$name] -or
            -not [long]::TryParse([string] $uploaded.id, [ref] $uploadedId) -or
            $uploadedId -le 0) {
            throw "GitHub returned mismatched metadata after uploading '$name'."
        }
        if ($uploadedIds.Contains($name) -or $uploadedId -cin @($uploadedIds.Values)) {
            throw "GitHub returned a duplicate name or ID after uploading '$name'."
        }
        $uploadedIds[$name] = $uploadedId
    }

    $draftAssets = Get-ReleaseAssets -ReleaseId $createdDraftReleaseId
    $draftAssetsByName = Assert-AssetSet `
        -Assets $draftAssets `
        -ExpectedNames $expectedFiles `
        -ExpectedSizes $internal.Sizes `
        -ExpectedIds $uploadedIds
    $draftDownload = Join-Path $tempRoot 'draft'
    Save-ReleaseAssets -AssetsByName $draftAssetsByName -ExpectedNames $expectedFiles -Destination $draftDownload
    $null = Assert-ReleaseSet `
        -Root $draftDownload `
        -ExpectedTag $Tag `
        -ExpectedRevision $SourceRevision `
        -ExpectedFiles $expectedFiles `
        -ArchiveName $archiveName `
        -ChecksumName $checksumName `
        -LockName $lockName `
        -EvidenceName $evidenceName
    Assert-DirectoryHashesEqual -ExpectedRoot $ReleaseDir -ActualRoot $draftDownload -Names $expectedFiles

    $publishPayload = [ordered]@{
        draft = [bool] $false
        prerelease = [bool] $true
    }
    Assert-LightweightTagTarget -ExpectedTag $Tag -ExpectedRevision $SourceRevision
    $publicationAttempted = $true
    $publishResponse = Invoke-GitHubJson `
        -Method Patch `
        -Uri "$script:ApiBase/releases/$createdDraftReleaseId" `
        -Body $publishPayload
    Assert-HttpStatus -Response $publishResponse -ExpectedStatus 200 -Operation "Publish release $createdDraftReleaseId"
    $published = Get-ResponseJson -Response $publishResponse -Operation "Publish release $createdDraftReleaseId"
    Assert-ReleaseObject `
        -Release $published `
        -ExpectedId $createdDraftReleaseId `
        -ExpectedDraft $false `
        -ExpectedTag $Tag `
        -ExpectedBody $releaseBody `
        -RequireImmutable

    $idResponse = Invoke-GitHubJson -Method Get -Uri "$script:ApiBase/releases/$createdDraftReleaseId"
    Assert-HttpStatus -Response $idResponse -ExpectedStatus 200 -Operation "Refetch published release by ID"
    $byId = Get-ResponseJson -Response $idResponse -Operation "Refetch published release by ID"
    Assert-ReleaseObject -Release $byId -ExpectedId $createdDraftReleaseId -ExpectedDraft $false -ExpectedTag $Tag -ExpectedBody $releaseBody -RequireImmutable

    $tagResponse = Invoke-GitHubJson -Method Get -Uri "$script:ApiBase/releases/tags/$encodedTag"
    Assert-HttpStatus -Response $tagResponse -ExpectedStatus 200 -Operation "Refetch published release by tag"
    $byTag = Get-ResponseJson -Response $tagResponse -Operation "Refetch published release by tag"
    Assert-ReleaseObject -Release $byTag -ExpectedId $createdDraftReleaseId -ExpectedDraft $false -ExpectedTag $Tag -ExpectedBody $releaseBody -RequireImmutable

    $publishedAssets = Get-ReleaseAssets -ReleaseId $createdDraftReleaseId
    $publishedAssetsByName = Assert-AssetSet `
        -Assets $publishedAssets `
        -ExpectedNames $expectedFiles `
        -ExpectedSizes $internal.Sizes `
        -ExpectedIds $uploadedIds
    $null = Assert-AssetSet -Assets @($byId.assets) -ExpectedNames $expectedFiles -ExpectedSizes $internal.Sizes -ExpectedIds $uploadedIds
    $null = Assert-AssetSet -Assets @($byTag.assets) -ExpectedNames $expectedFiles -ExpectedSizes $internal.Sizes -ExpectedIds $uploadedIds

    $publishedDownload = Join-Path $tempRoot 'published'
    Save-ReleaseAssets -AssetsByName $publishedAssetsByName -ExpectedNames $expectedFiles -Destination $publishedDownload
    $null = Assert-ReleaseSet `
        -Root $publishedDownload `
        -ExpectedTag $Tag `
        -ExpectedRevision $SourceRevision `
        -ExpectedFiles $expectedFiles `
        -ArchiveName $archiveName `
        -ChecksumName $checksumName `
        -LockName $lockName `
        -EvidenceName $evidenceName
    Assert-DirectoryHashesEqual -ExpectedRoot $ReleaseDir -ActualRoot $publishedDownload -Names $expectedFiles
    Assert-DirectoryHashesEqual -ExpectedRoot $draftDownload -ActualRoot $publishedDownload -Names $expectedFiles

    $finalIdResponse = Invoke-GitHubJson -Method Get -Uri "$script:ApiBase/releases/$createdDraftReleaseId"
    Assert-HttpStatus -Response $finalIdResponse -ExpectedStatus 200 -Operation 'Final release verification by ID'
    $finalById = Get-ResponseJson -Response $finalIdResponse -Operation 'Final release verification by ID'
    Assert-ReleaseObject -Release $finalById -ExpectedId $createdDraftReleaseId -ExpectedDraft $false -ExpectedTag $Tag -ExpectedBody $releaseBody -RequireImmutable
    $null = Assert-AssetSet -Assets @($finalById.assets) -ExpectedNames $expectedFiles -ExpectedSizes $internal.Sizes -ExpectedIds $uploadedIds

    $finalTagResponse = Invoke-GitHubJson -Method Get -Uri "$script:ApiBase/releases/tags/$encodedTag"
    Assert-HttpStatus -Response $finalTagResponse -ExpectedStatus 200 -Operation 'Final release verification by tag'
    $finalByTag = Get-ResponseJson -Response $finalTagResponse -Operation 'Final release verification by tag'
    Assert-ReleaseObject -Release $finalByTag -ExpectedId $createdDraftReleaseId -ExpectedDraft $false -ExpectedTag $Tag -ExpectedBody $releaseBody -RequireImmutable
    $null = Assert-AssetSet -Assets @($finalByTag.assets) -ExpectedNames $expectedFiles -ExpectedSizes $internal.Sizes -ExpectedIds $uploadedIds

    $transactionComplete = $true
    Write-Output "Published release '$Tag' as ID $createdDraftReleaseId with four verified immutable assets."
} finally {
    if (-not $transactionComplete -and $null -ne $createdDraftReleaseId -and $publicationAttempted) {
        Write-Warning "Publication was attempted; preserving captured release ID $createdDraftReleaseId because the outcome may be ambiguous and an immutable release tag name cannot be reused."
    } elseif (-not $transactionComplete -and $null -ne $createdDraftReleaseId) {
        try {
            $cleanupUri = "$script:ApiBase/releases/$createdDraftReleaseId"
            $cleanupLookup = Invoke-GitHubJson -Method Get -Uri $cleanupUri
            if ([int] $cleanupLookup.StatusCode -eq 404) {
                Write-Output "Captured draft release ID $createdDraftReleaseId is already absent."
            } elseif ([int] $cleanupLookup.StatusCode -ne 200) {
                Write-Warning "Preserving captured release ID $createdDraftReleaseId because draft state could not be proven (HTTP $($cleanupLookup.StatusCode))."
            } else {
                $cleanupRelease = Get-ResponseJson -Response $cleanupLookup -Operation "Inspect captured release $createdDraftReleaseId before cleanup"
                $isOwnedDraft = (
                    [long] $cleanupRelease.id -eq $createdDraftReleaseId -and
                    [string] $cleanupRelease.tag_name -ceq $Tag -and
                    $cleanupRelease.draft -is [bool] -and
                    $cleanupRelease.draft -eq $true -and
                    $cleanupRelease.immutable -is [bool] -and
                    $cleanupRelease.immutable -eq $false
                )
                if (-not $isOwnedDraft) {
                    Write-Warning "Preserving captured release ID $createdDraftReleaseId because it is not confirmed as the mutable unpublished draft created by this run."
                } else {
                    $deleteResponse = Invoke-GitHubJson -Method Delete -Uri $cleanupUri
                    if ([int] $deleteResponse.StatusCode -notin @(204, 404)) {
                        Write-Warning "Cleanup of captured draft release ID $createdDraftReleaseId failed with HTTP $($deleteResponse.StatusCode)."
                    } else {
                        Write-Output "Cleaned up captured unpublished draft release ID $createdDraftReleaseId after pre-publication failure."
                    }
                }
            }
        } catch {
            Write-Warning "Cleanup inspection for captured release ID $createdDraftReleaseId failed: $($_.Exception.Message)"
        }
    }

    $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $resolvedPublishRoot = [System.IO.Path]::GetFullPath($tempRoot)
    if ($resolvedPublishRoot.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedPublishRoot) -like 'libvirtualgamepad-publish-*') {
        Remove-Item -LiteralPath $resolvedPublishRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
