# skillcli

## Install

### GitHub Copilot CLI — native marketplace

```text
/plugin marketplace add WillEastbury/skillcli
/plugin install skillcli-skill-zero@skillcli
```

Marketplace: [github.com/WillEastbury/skillcli](https://github.com/WillEastbury/skillcli)

### Scout, Co-Work, and cross-host CLI

Windows:

```powershell
$p=Join-Path $env:TEMP 'skillcli-install.ps1'; iwr -UseBasicParsing -Headers @{Accept='application/vnd.github.raw+json';'User-Agent'='skillcli'} 'https://api.github.com/repos/WillEastbury/skillcli/contents/install.ps1?ref=main' -OutFile $p; Unblock-File $p; & $p; Remove-Item $p
```

macOS or Linux:

```bash
p="$(mktemp)"; curl -fsSL "https://raw.githubusercontent.com/WillEastbury/skillcli/main/install.sh" -o "$p"; bash "$p"; rm -f "$p"
```

`skillcli` is an MIT-licensed, agent-friendly CLI for searching and installing governed
AI Markdown skills from one or more GitHub catalogues.

The agent remains the user interface. The CLI provides four deterministic commands:

```text
skillcli search --role seller --query "prompt quality"
skillcli install --skill WillEastbury/skillcli/skillcli-prompt-quality-check
skillcli remove --skill WillEastbury/skillcli/skillcli-prompt-quality-check
skillcli update --skill WillEastbury/skillcli/skillcli-prompt-quality-check
skillcli update --all
```

## Private downstream install

Layer a private catalogue over the public source:

```powershell
$env:SKILLCLI_SOURCES_REPOSITORY='your-org/private-skills'; $p=Join-Path $env:TEMP 'skillcli-install.ps1'; iwr -UseBasicParsing -Headers @{Accept='application/vnd.github.raw+json';'User-Agent'='skillcli'} 'https://api.github.com/repos/WillEastbury/skillcli/contents/install.ps1?ref=main' -OutFile $p; Unblock-File $p; & $p; Remove-Item $p
```

The installer downloads `skillcli`, adds it to the user PATH, and installs Skill Zero
into detected:

- GitHub Copilot CLI: `%USERPROFILE%\.copilot\skills`
- Scout: `%USERPROFILE%\.scout\m-skills`
- Copilot Co-Work: `%OneDrive%\Documents\Cowork\Skills`

On macOS, Scout uses `~/.copilot/m-skills` and Co-Work discovery checks
`~/Library/CloudStorage/OneDrive-*/Documents/Cowork/Skills`.

The Co-Work copy includes a local catalogue snapshot for import through Customize.

## Multiple catalogues

`sources.json` beside the installed CLI determines which catalogues are combined:

```json
{
  "sources": [
    {
      "id": "public",
      "repository": "WillEastbury/skillcli",
      "ref": "main",
      "private": false
    },
    {
      "id": "company",
      "repository": "your-org/private-skills",
      "ref": "main",
      "private": true
    }
  ]
}
```

Search merges all accessible catalogues and labels each result with its source.
Every skill is addressed as `OWNER/REPO/skill-id`, so catalogues cannot collide. An
inaccessible private source produces a warning while accessible public sources continue
to work.

Private catalogues use the active authenticated `gh` account. Public catalogues can be
read anonymously.

## Agent orchestration

Skill Zero asks for role and need, runs `skillcli search`, presents the returned table,
and runs install/remove/update only after an explicit user request.

Skill One runs a search first to avoid duplicate submissions. If no existing skill fits,
it can continue with the repository's governed issue and review workflow.

## Native Copilot CLI marketplace

```text
/plugin marketplace add WillEastbury/skillcli
/plugin install skillcli-skill-zero@skillcli
```

When native Copilot CLI is detected, `skillcli install`, `update`, and `remove` delegate
to these native plugin commands. Scout and Co-Work use the same plugin packages through
filesystem adapters.

## Catalogue format

The native marketplace structure is authoritative:

- `.github/plugin/marketplace.json`
- `plugins/<plugin-name>/plugin.json`
- `plugins/<plugin-name>/skillcli.json`
- `plugins/<plugin-name>/skills/...`

`skills.json` is generated as a temporary compatibility/search index. See:

- [`schemas/skills.schema.json`](schemas/skills.schema.json)
- [`schemas/sources.schema.json`](schemas/sources.schema.json)
- [`schemas/catalogue.schema.json`](schemas/catalogue.schema.json)
- [`schemas/skillcli-plugin.schema.json`](schemas/skillcli-plugin.schema.json)
- [`SKILLS.md`](SKILLS.md)

## Core skills

<!-- BEGIN GENERATED SKILL CATALOG -->
| ID | Name | Version | Status | Requirements | Description |
| --- | --- | --- | --- | --- | --- |
| `WillEastbury/skillcli/skillcli-skill-one` | Skill One: GitHub Intake | 4.0.0 | approved | network: github.com, api.github.com; filesystem: read-write; shell: git, skillcli; auth: host-managed GitHub access | Submits a proposed Markdown skill and supporting files on a review branch and opens a governed GitHub issue. |
| `WillEastbury/skillcli/skillcli-skill-two` | Skill Two: Guided Skill Builder | 2.0.1 | approved | filesystem: read-write; MCP: dataverse, m365, workiq, other user-approved MCP servers; auth: host-managed MCP access | Builds a new reusable skill from user-approved enterprise evidence when no catalogue skill is suitable. |
| `WillEastbury/skillcli/skillcli-skill-zero` | Skill Zero: Library Browser | 6.0.0 | approved | network: github.com, api.github.com; filesystem: read-write; shell: copilot, gh, skillcli; auth: host-managed GitHub access | Orchestrates the managed skillcli command to search, install, remove, and update approved skills across detected agent hosts. |
<!-- END GENERATED SKILL CATALOG -->

## Governance

- Catalogue changes use pull requests, CODEOWNERS, and CI.
- Only approved catalogue entries are installable.
- Installation downloads only files declared in `skills.json` and verifies each SHA-256
  checksum before writing.
- Windows path aliases, reserved names, reparse points, and case-folded collisions are
  rejected.
- Installed source metadata binds updates and removals to the original repository
  namespace.
- Destination conflicts and unexpected local files are refused.
- No dependency is installed automatically.
- Private catalogue content remains in the private repository; this upstream contains
  only reusable public tooling and skills.

## Develop

```powershell
python tools\validate_manifest.py
python tools\render_marketplace.py
python tools\render_readme.py
python tools\render_readme.py --check
python -m unittest discover -s tests -v
```

External inventories can be transformed into review candidates with
`tools/normalize_inventory.py`.

## Licence

MIT
