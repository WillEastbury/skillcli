# skillcli

## Install

### Prerequisites

| Prerequisite | Required? | Why |
| --- | --- | --- |
| **Python 3.10 or newer** | **Always** | `skillcli` is a Python CLI. The installer stops if `python` is missing or older than 3.10. |
| **GitHub CLI (`gh`), authenticated** | **Only for private catalogues** | Needed to read private/EMU catalogues such as the Digital Native Skills Library. **Without `gh` you can still use every public skill** — private sources are simply skipped with a warning. |
| Windows PowerShell 5.1+ or PowerShell 7 | Windows installer only | Runs `install.ps1`. |
| Bash and `curl` | macOS/Linux installer only | Runs `install.sh`. |
| GitHub Copilot CLI signed in to GitHub | Native marketplace route only | Uses `/plugin marketplace add` instead of the standalone installer. |

Public marketplaces and plugins are downloaded anonymously over HTTPS, so no GitHub
token is needed for the public catalogue.

Check what you have:

```powershell
python --version   # need 3.10 or newer
gh auth status     # optional: only needed for private catalogues
```

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
skillcli self-update
```

## Add the private Digital Native Skills Library

The public installer sets up **only** the public marketplace. To also get the private
Microsoft EMU catalogue:

**Repository:**
[github.com/wieastbu_microsoft/DigitalNativeSkillsLibrary](https://github.com/wieastbu_microsoft/DigitalNativeSkillsLibrary)

**Step 1 — sign in to the EMU account with GitHub CLI** (one time):

```text
gh auth login --hostname github.com
gh auth status
```

You must be signed in as an account that has access to the private repository.

**Step 2 — register the catalogue:**

```text
skillcli register wieastbu_microsoft/DigitalNativeSkillsLibrary
```

**Step 3 — confirm it worked:**

```text
skillcli search --role seller --query "prompt quality"
```

Results are labelled with the marketplace they came from, so private skills are easy to
spot.

If `gh` is missing or not authorised for that repository, `skillcli` prints a warning and
carries on with the public catalogue only — nothing breaks, you just do not see the
private skills.

### Register any other marketplace

```text
skillcli register OWNER/REPO
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
it can continue with the repository's governed review workflow.

## Contribute a skill

Submissions go straight into the catalogue's own `plugins/` tree, so **approving and
merging the pull request publishes the skill immediately** — there is no separate
promote step.

1. Skill Two builds the plugin package (`plugin.json`, `skillcli.json`,
   `skills/<skill-id>/SKILL.md`) with `review.state` set to `pending`.
2. Skill One puts it on a `skill-intake/<skill-id>-<timestamp>` branch, registers it in
   `.github/plugin/marketplace.json`, regenerates the catalogue views, and opens a pull
   request.
3. CI validates checksums, schemas, generated file freshness, and security tests.
4. A maintainer approves, flips the review state, and merges.

The catalogue table row is rendered from plugin metadata, so it appears in the pull
request diff. Never hand-edit `skills.json`, `SKILLS.md`, or the block between the
`BEGIN GENERATED SKILL CATALOG` and `END GENERATED SKILL CATALOG` markers.

Submit to whichever repository owns the skill — this public catalogue or a private one —
never both.

### Maintainer approval

A plugin version is declared in three files that must agree, and the review state lives
in a fourth. One command keeps them in sync and regenerates everything:

```powershell
python tools\sync_plugin_metadata.py --plugin <plugin-name> --state approved --reviewer "<maintainer>"
python tools\render_readme.py
python tools\validate_manifest.py
```

Bump a version the same way:

```powershell
python tools\sync_plugin_metadata.py --plugin <plugin-name> --version 1.2.0
python tools\render_readme.py
```

### Publishing without GitHub Actions

Catalogues that cannot run GitHub Actions publish from a client machine instead.
`tools/publish_skill.py` runs every check CI would run, regenerates the catalogue views,
and only then creates the branch and pull request:

```powershell
python tools\publish_skill.py --plugin <plugin-name> --state pending --dry-run
python tools\publish_skill.py --plugin <plugin-name> --state pending
```

It refuses to push if validation fails, so merging stays equivalent to publishing. Select
an account explicitly with `--gh-user`; the script never switches the shared active `gh`
account. If the pull request cannot be created after the push, the branch is retained and
reported for recovery.

## Update

Update the CLI itself:

```text
skillcli self-update
```

Update installed skills:

```text
skillcli update --skill WillEastbury/skillcli/skillcli-skill-zero
skillcli update --all
```

`update --all` refreshes every installed skill from the repository it was installed
from, across each detected host. Re-running the installer is not required for routine
updates; use `self-update` instead.

Under native GitHub Copilot CLI these delegate to `copilot plugin install` against the
registered marketplace.

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

The installer sets up `skillcli` and **Skill Zero only**. Skill One and Skill Two are
catalogue entries that Skill Zero installs on demand, after you ask for them:

```text
skillcli install --skill WillEastbury/skillcli/skillcli-skill-one
skillcli install --skill WillEastbury/skillcli/skillcli-skill-two
```

<!-- BEGIN GENERATED SKILL CATALOG -->
| ID | Name | Version | Status | Requirements | Description |
| --- | --- | --- | --- | --- | --- |
| `WillEastbury/skillcli/skillcli-skill-one` | Skill One: GitHub Intake | 5.0.0 | approved | network: github.com, api.github.com; filesystem: read-write; shell: git, skillcli; auth: host-managed GitHub access | Submits a proposed Markdown skill as a ready-to-merge plugin package on a review branch and opens a governed pull request. |
| `WillEastbury/skillcli/skillcli-skill-two` | Skill Two: Guided Skill Builder | 3.0.0 | approved | filesystem: read-write; MCP: dataverse, m365, workiq, other user-approved MCP servers; auth: host-managed MCP access | Builds a new reusable skill from user-approved enterprise evidence when no catalogue skill is suitable. |
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
python tools\sync_plugin_metadata.py --plugin <plugin-name> --version 1.2.0
python -m unittest discover -s tests -v
```

External inventories can be transformed into review candidates with
`tools/normalize_inventory.py`.

## Licence

MIT
