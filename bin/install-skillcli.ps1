[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateLength(1, 511)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*$')]
    [string]$Source
)

$ErrorActionPreference = 'Stop'
$uri = 'https://raw.githubusercontent.com/WillEastbury/skillcli/589f6a8721ce86b0894bd813943be82581fdeefc/bin/skillcli.exe'
$expected = 'D27E852A6F11DCE507BB6D18311F291029DEFDEB58A8A9B191EF35D8E61426FA'
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
