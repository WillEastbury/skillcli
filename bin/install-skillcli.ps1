[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateLength(1, 511)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$')]
    [string]$Source
)

$ErrorActionPreference = 'Stop'
$uri = 'https://raw.githubusercontent.com/WillEastbury/skillcli/627e6271c02b92b0d254fa7d264961a29ef0b330/bin/skillcli.exe'
$expected = '8D343D793AB15EC3D4E93855EAAC7E41FF5FB491D6DB869D8557D07162B9CC9D'
$directory = Join-Path $env:LOCALAPPDATA 'SkillCLI\Bootstrap'
$installer = Join-Path $directory 'skillcli.exe'

try {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    Invoke-WebRequest $uri -OutFile $installer
    if ((Get-FileHash $installer -Algorithm SHA256).Hash -ne $expected) {
        throw 'SkillCLI checksum verification failed.'
    }
    Unblock-File -LiteralPath $installer
    & $installer setup --source $Source
    if ($LASTEXITCODE) {
        throw "SkillCLI setup failed with exit code $LASTEXITCODE."
    }
} finally {
    Remove-Item $installer -Force -ErrorAction SilentlyContinue
}
