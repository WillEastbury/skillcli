---
name: "Skill Zero: Library Browser"
description: "Orchestrates the managed skillcli command to search, install, remove, and update approved skills across detected agent hosts."
version: "4.0.2"
skill-id: "skill-zero"
---

# Skill Zero: Library Browser

## Purpose

Use the current AI agent as the interface to the governed skill library by orchestrating
the trusted `skillcli` command.

Do not reproduce catalogue or installation logic in conversation. Run the CLI and
interpret its table output for the user.

This skill must be installed through the host's trusted native or administrator-managed
skill deployment mechanism. Repository content cannot bootstrap or authorise its own
installation.

## Search

When the user asks to find or scan for skills, gather their role and query, then run:

```text
skillcli search --role <role-id> --query "<user need>"
```

Present the returned table. Do not show more results than the CLI returns.

## Install

On an explicit user selection, run:

```text
skillcli install --skill <skill-id>
```

The CLI installs to every detected Copilot CLI, Scout managed-skills, and Co-Work
OneDrive skills location. Report its result table exactly. Do not manually copy files.

## Updates

Run one of:

```text
skillcli update --skill <skill-id>
skillcli update --all
```

Only run update after an explicit user request.

## No match

If no suitable skill exists, explain the closest results and offer Skill Two. Do not
create a new skill until the user agrees.

## Removal

On an explicit removal request, run:

```text
skillcli remove --skill <skill-id>
```

Report any identity mismatch or local-file refusal rather than bypassing it.
