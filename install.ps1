$ErrorActionPreference = 'Stop'

$PublicRepository = 'WillEastbury/skillcli'
$PublicRef = 'main'
$SourcesRepository = if ($env:SKILLCLI_SOURCES_REPOSITORY) {
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
    throw 'Python 3 is required.'
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
        -Uri "https://raw.githubusercontent.com/$Repository/$Commit/$Path" `
        -Headers @{ 'User-Agent' = 'skillcli-installer' }).Content
}

$publicCommit = Get-PublicCommit $PublicRepository $PublicRef
$sourceIsPrivate = $SourcesRepository -ne $PublicRepository

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

$cliContent = Get-RepositoryFile `
    $PublicRepository `
    $publicCommit `
    'skills/skill-zero/skillcli.py' `
    $false
$sourcesContent = Get-RepositoryFile `
    $SourcesRepository `
    $sourcesCommit `
    'skill-sources.json' `
    $sourceIsPrivate

New-Item -ItemType Directory -Force -Path $ToolDirectory | Out-Null
$cliPath = Join-Path $ToolDirectory 'skillcli.py'
$wrapperPath = Join-Path $ToolDirectory 'skillcli.cmd'
$sourcesPath = Join-Path $ToolDirectory 'sources.json'

[IO.File]::WriteAllText(
    $cliPath,
    [string]$cliContent,
    [Text.UTF8Encoding]::new($false)
)
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

& $wrapperPath install --skill skill-zero
if ($LASTEXITCODE -ne 0) {
    throw 'Skill Zero installation failed.'
}

Write-Output ''
Write-Output 'Installed: skillcli and Skill Zero'
Write-Output "CLI commit: $publicCommit"
Write-Output "Catalogue configuration: $SourcesRepository@$sourcesCommit"
Write-Output 'Try: skillcli search --role seller --query "prompt quality"'
