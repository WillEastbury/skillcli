$ErrorActionPreference = 'Stop'

$PublicRepository = 'WillEastbury/skillcli'
$PublicRef = if ($env:SKILLCLI_PUBLIC_REF) {
    $env:SKILLCLI_PUBLIC_REF
} else {
    'main'
}
$privateMode = $env:SKILLCLI_PRIVATE_MODE -eq '1'
$SourcesRepository = if ($privateMode -and $env:SKILLCLI_SOURCES_REPOSITORY) {
    $env:SKILLCLI_SOURCES_REPOSITORY
} else {
    $PublicRepository
}
$SourcesRef = if ($env:SKILLCLI_SOURCES_REF) {
    $env:SKILLCLI_SOURCES_REF
} else {
    'main'
}
$ToolDirectory = if ($env:SKILLCLI_TOOL_DIRECTORY) {
    $env:SKILLCLI_TOOL_DIRECTORY
} else {
    Join-Path $env:LOCALAPPDATA 'skillcli'
}

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    throw 'Python 3.10 or newer is required.'
}
$pythonVersion = & python -c "import sys; print('.'.join(map(str, sys.version_info[:3])))"
if ($LASTEXITCODE -ne 0 -or [version]$pythonVersion -lt [version]'3.10') {
    throw "Python 3.10 or newer is required. Found: $pythonVersion"
}
if ($SourcesRepository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw 'SKILLCLI_SOURCES_REPOSITORY must use OWNER/REPO format.'
}

function Get-PublicCommit([string]$Repository, [string]$Ref) {
    $headers = @{ 'User-Agent' = 'skillcli-installer' }
    $response = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/$Repository/commits/$Ref" `
        -Headers $headers
    return $response.sha
}

function Get-RepositoryFile(
    [string]$Repository,
    [string]$Commit,
    [string]$Path,
    [bool]$Private
) {
    if ($Private) {
        if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
            throw "GitHub CLI is required for private catalogue $Repository."
        }
        $content = & gh api `
            -H 'Accept: application/vnd.github.raw+json' `
            "repos/$Repository/contents/$Path`?ref=$Commit"
        if ($LASTEXITCODE -ne 0 -or -not $content) {
            throw "Cannot download $Path from private catalogue $Repository."
        }
        return ($content -join "`n") + "`n"
    }
    return (Invoke-WebRequest `
        -UseBasicParsing `
        -Uri "https://raw.githubusercontent.com/$Repository/$Commit/$Path" `
        -Headers @{ 'User-Agent' = 'skillcli-installer' }).Content
}

$publicCommit = Get-PublicCommit $PublicRepository $PublicRef
$sourceIsPrivate = $privateMode -and $SourcesRepository -ne $PublicRepository

if ($sourceIsPrivate) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw 'GitHub CLI is required for the configured private catalogue.'
    }
    $sourcesCommit = & gh api `
        "repos/$SourcesRepository/commits/$SourcesRef" `
        --jq '.sha' 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $sourcesCommit) {
        throw "Cannot access private catalogue $SourcesRepository. Switch gh to an authorised account."
    }
} else {
    $sourcesCommit = Get-PublicCommit $SourcesRepository $SourcesRef
}

$pluginPath = 'plugins/skillcli-skill-zero'
$metadataContent = Get-RepositoryFile `
    $PublicRepository `
    $publicCommit `
    "$pluginPath/skillcli.json" `
    $false
$pluginMetadata = $metadataContent | ConvertFrom-Json
$toolFiles = @($pluginMetadata.files | Where-Object { $_.target -eq 'tool' })
if ($toolFiles.Count -lt 2) {
    throw 'Skill Zero plugin does not declare the required CLI tool files.'
}
$sourcesContent = Get-RepositoryFile `
    $SourcesRepository `
    $sourcesCommit `
    'skill-sources.json' `
    $sourceIsPrivate

New-Item -ItemType Directory -Force -Path $ToolDirectory | Out-Null
$cliPath = Join-Path $ToolDirectory 'skillcli.py'
$corePath = Join-Path $ToolDirectory 'skillcli_core.py'
$wrapperPath = Join-Path $ToolDirectory 'skillcli.cmd'
$sourcesPath = Join-Path $ToolDirectory 'sources.json'

foreach ($record in $toolFiles) {
    $content = Get-RepositoryFile `
        $PublicRepository `
        $publicCommit `
        "$pluginPath/$($record.path)" `
        $false
    $normalized = ([string]$content).Replace("`r`n", "`n").Replace("`r", "`n")
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($normalized)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha256.ComputeHash($bytes)
    } finally {
        $sha256.Dispose()
    }
    $actual = -join ($hash | ForEach-Object { $_.ToString('x2') })
    if ($actual -ne $record.sha256) {
        throw "Checksum mismatch for $($record.path)."
    }
    $destination = switch ([IO.Path]::GetFileName($record.path)) {
        'skillcli.py' { $cliPath }
        'skillcli_core.py' { $corePath }
        default { throw "Unexpected Skill Zero tool file: $($record.path)" }
    }
    [IO.File]::WriteAllBytes($destination, $bytes)
}
[IO.File]::WriteAllText(
    $sourcesPath,
    [string]$sourcesContent,
    [Text.UTF8Encoding]::new($false)
)
[IO.File]::WriteAllText(
    $wrapperPath,
    "@echo off`r`npython `"%~dp0skillcli.py`" %*`r`n",
    [Text.ASCIIEncoding]::new()
)

if ($env:SKILLCLI_NO_PATH_UPDATE -ne '1') {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $pathParts = @($userPath -split ';' | Where-Object { $_ })
    if ($pathParts -notcontains $ToolDirectory) {
        [Environment]::SetEnvironmentVariable(
            'Path',
            ((@($pathParts) + $ToolDirectory) -join ';'),
            'User'
        )
    }
}
$env:Path = "$ToolDirectory;$env:Path"

& $wrapperPath update --skill WillEastbury/skillcli/skillcli-skill-zero
if ($LASTEXITCODE -ne 0) {
    throw 'Skill Zero installation failed.'
}

Write-Output ''
Write-Output 'Installed: skillcli and Skill Zero'
Write-Output "CLI commit: $publicCommit"
Write-Output "Catalogue configuration: $SourcesRepository@$sourcesCommit"
Write-Output 'Try: skillcli search --role seller --query "prompt quality"'
