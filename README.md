# skillcli

## Install

```powershell
$p=Join-Path $env:TEMP 'skillcli-install.ps1'; iwr -Headers @{Accept='application/vnd.github.raw+json';'User-Agent'='skillcli'} 'https://api.github.com/repos/WillEastbury/skillcli/contents/install.ps1?ref=main' -OutFile $p; Unblock-File $p; & $p; Remove-Item $p
```

`skillcli` is an MIT-licensed, agent-friendly CLI for searching and installing governed
AI Markdown skills from one or more GitHub catalogues.

The agent remains the user interface. The CLI provides four deterministic commands:

```text
skillcli search --role seller --query "prompt quality"
skillcli install --skill prompt-quality-check
skillcli remove --skill prompt-quality-check
skillcli update --skill prompt-quality-check
skillcli update --all
```

## Private downstream install

Layer a private catalogue over the public source:

```powershell
$env:SKILLCLI_SOURCES_REPOSITORY='your-org/private-skills'; $p=Join-Path $env:TEMP 'skillcli-install.ps1'; iwr -Headers @{Accept='application/vnd.github.raw+json';'User-Agent'='skillcli'} 'https://api.github.com/repos/WillEastbury/skillcli/contents/install.ps1?ref=main' -OutFile $p; Unblock-File $p; & $p; Remove-Item $p
```

The installer downloads `skillcli`, adds it to the user PATH, and installs Skill Zero
into detected:

- GitHub Copilot CLI: `%USERPROFILE%\.copilot\skills`
- Scout: `%USERPROFILE%\.scout\m-skills`
- Copilot Co-Work: `%OneDrive%\Documents\Cowork\Skills`

The Co-Work copy includes a local catalogue snapshot for import through Customize.

## Multiple catalogues

`sources.json` beside the installed CLI determines which catalogues are combined:

```json
{
  "duplicatePolicy": "highest-priority",
  "sources": [
    {
      "id": "public",
      "repository": "WillEastbury/skillcli",
      "ref": "main",
      "priority": 10,
      "private": false
    },
    {
      "id": "company",
      "repository": "your-org/private-skills",
      "ref": "main",
      "priority": 100,
      "private": true
    }
  ]
}
```

Search merges all accessible catalogues and labels each result with its source. Higher
priority wins when the same skill ID appears more than once. An inaccessible private
source produces a warning while accessible public sources continue to work.

Private catalogues use the active authenticated `gh` account. Public catalogues can be
read anonymously.

## Agent orchestration

Skill Zero asks for role and need, runs `skillcli search`, presents the returned table,
and runs install/remove/update only after an explicit user request.

Skill One runs a search first to avoid duplicate submissions. If no existing skill fits,
it can continue with the repository's governed issue and review workflow.

## Catalogue format

`skills.json` is the source of truth. Each approved entry points to a folder containing
`SKILL.md` and optional supporting files. See:

- [`schemas/skills.schema.json`](schemas/skills.schema.json)
- [`schemas/sources.schema.json`](schemas/sources.schema.json)
- [`SKILLS.md`](SKILLS.md)

## Core skills

<!-- BEGIN GENERATED SKILL CATALOG -->
| ID | Name | Version | Status | Requirements | Description |
| --- | --- | --- | --- | --- | --- |
| `skill-one` | Skill One: GitHub Intake | 2.1.1 | approved | network: github.com, api.github.com; filesystem: read-write; shell: git, skillcli; auth: host-managed GitHub access | Submits a proposed Markdown skill and supporting files on a review branch and opens a governed GitHub issue. |
| `skill-two` | Skill Two: Guided Skill Builder | 2.0.1 | approved | filesystem: read-write; MCP: dataverse, m365, workiq, other user-approved MCP servers; auth: host-managed MCP access | Builds a new reusable skill from user-approved enterprise evidence when no catalogue skill is suitable. |
| `skill-zero` | Skill Zero: Library Browser | 4.0.2 | approved | network: github.com, api.github.com; filesystem: read-write; shell: gh, skillcli; auth: host-managed GitHub access | Orchestrates the managed skillcli command to search, install, remove, and update approved skills across detected agent hosts. |
<!-- END GENERATED SKILL CATALOG -->

## Governance

- Catalogue changes use pull requests, CODEOWNERS, and CI.
- Only approved catalogue entries are installable.
- Installation copies the complete governed folder from one Git commit.
- Destination conflicts and unexpected local files are refused.
- No dependency is installed automatically.
- Private catalogue content remains in the private repository; this upstream contains
  only reusable public tooling and skills.

## Develop

```powershell
python tools\validate_manifest.py
python tools\render_readme.py
python tools\render_readme.py --check
```

External inventories can be transformed into review candidates with
`tools/normalize_inventory.py`.

## Licence

MIT
