$ErrorActionPreference = 'Stop'

$PublicRepository = 'WillEastbury/skillcli'
$PublicRef = if ($env:SKILLCLI_PUBLIC_REF) {
    $env:SKILLCLI_PUBLIC_REF
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
    [string]$Path
) {
    return (Invoke-WebRequest `
        -UseBasicParsing `
        -Uri "https://raw.githubusercontent.com/$Repository/$Commit/$Path" `
        -Headers @{ 'User-Agent' = 'skillcli-installer' }).Content
}

$publicCommit = Get-PublicCommit $PublicRepository $PublicRef

$pluginPath = 'plugins/skillcli-skill-zero'
$metadataContent = Get-RepositoryFile `
    $PublicRepository `
    $publicCommit `
    "$pluginPath/skillcli.json"
$pluginMetadata = $metadataContent | ConvertFrom-Json
$toolFiles = @($pluginMetadata.files | Where-Object { $_.target -eq 'tool' })
if ($toolFiles.Count -lt 2) {
    throw 'Skill Zero plugin does not declare the required CLI tool files.'
}
$sourcesContent = Get-RepositoryFile `
    $PublicRepository `
    $publicCommit `
    'skill-sources.json'

New-Item -ItemType Directory -Force -Path $ToolDirectory | Out-Null
$cliPath = Join-Path $ToolDirectory 'skillcli.py'
$corePath = Join-Path $ToolDirectory 'skillcli_core.py'
$wrapperPath = Join-Path $ToolDirectory 'skillcli.cmd'
$sourcesPath = Join-Path $ToolDirectory 'sources.json'

foreach ($record in $toolFiles) {
    $content = Get-RepositoryFile `
        $PublicRepository `
        $publicCommit `
        "$pluginPath/$($record.path)"
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
Write-Output "Catalogue configuration: $PublicRepository@$publicCommit"
Write-Output 'Try: skillcli search --role seller --query "prompt quality"'
