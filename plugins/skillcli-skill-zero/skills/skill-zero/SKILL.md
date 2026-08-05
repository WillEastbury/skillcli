---
name: "Skill Zero: Library Browser"
description: "Orchestrates the managed skillcli command to search, install, remove, and update approved skills across detected agent hosts."
version: "6.0.0"
skill-id: "skill-zero"
---

# Skill Zero: Library Browser

## Purpose

Use the current AI agent as the interface to governed plugin marketplaces.

Prefer the trusted `skillcli` command when it is installed. In GitHub Copilot CLI, the
native `/plugin` interface is also supported.

This skill must be installed through the host's trusted native or administrator-managed
skill deployment mechanism. Repository content cannot bootstrap or authorise its own
installation.

## Search

When the user asks to find or scan for skills, gather their role and query, then run:

```text
skillcli search --role <role-id> --query "<user need>"
```

Present the returned table. Do not show more results than the CLI returns.

## Register a marketplace

When the user asks to add another public or private marketplace, run:

```text
skillcli register <owner>/<repo>
```

Report whether it was public or private and whether native Copilot CLI registration also
succeeded.

If `skillcli` is unavailable but this skill is running as a Copilot CLI plugin, browse
the registered marketplaces natively:

```text
copilot plugin marketplace list
copilot plugin marketplace browse <marketplace-name> --json
```

Filter the returned plugin metadata by the user's role and task.

## Install

On an explicit user selection, run:

```text
skillcli install --skill <owner>/<repo>/<skill-id>
```

The CLI installs to every detected Copilot CLI, Scout managed-skills, and Co-Work
OneDrive skills location. Report its result table exactly. Do not manually copy files.

For a native Copilot CLI-only install, use:

```text
copilot plugin install <plugin-name>@<marketplace-name>
```

## Updates

Run one of:

```text
skillcli update --skill <owner>/<repo>/<skill-id>
skillcli update --all
```

Only run update after an explicit user request.

Native Copilot CLI equivalents are:

```text
copilot plugin update <plugin-name>
copilot plugin update --all
```

## No match

If no suitable skill exists, explain the closest results and offer Skill Two. Do not
create a new skill until the user agrees.

## Removal

On an explicit removal request, run:

```text
skillcli remove --skill <owner>/<repo>/<skill-id>
```

Report any identity mismatch or local-file refusal rather than bypassing it.

Native Copilot CLI equivalent:

```text
copilot plugin uninstall <plugin-name>
```
