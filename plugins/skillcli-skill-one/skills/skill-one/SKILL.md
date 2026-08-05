---
name: "Skill One: GitHub Intake"
description: "Submits a proposed Markdown skill and supporting files on a review branch and opens a governed GitHub issue."
version: "4.1.0"
skill-id: "skill-one"
---

# Skill One: GitHub Intake

## Purpose

Submit a proposed Markdown skill and any supporting files for review. The complete
proposal goes on an isolated review branch, and a GitHub issue links to that branch.

Submission does not approve, publish, merge, or install the skill.

## Procedure

1. Run `skillcli search --role skill-author --query "<proposed purpose>"`, or use native
   `copilot plugin marketplace browse <marketplace> --json`, to check for an existing
   approved plugin.
2. If the user selects an existing result, run
   `skillcli install --skill <owner>/<repo>/<skill-id>` and stop the submission flow.
3. Otherwise ask the user to confirm the target `OWNER/REPO` selected during Skill Two.
   If no repository was selected, stop and ask; never infer one.
4. Confirm the proposed lowercase kebab-case skill ID.
5. Review the proposed folder and require `SKILL.md`.
6. Reject secrets, credentials, personal data, customer data, hidden files, symbolic
   links, binaries, and unrelated files.
7. Summarise the purpose, intended roles, categories, runtime, dependencies,
   capabilities, MCP servers, authentication, source, and licence.
8. Show the selected repository, branch, and issue plan.
9. An explicit request to submit is approval to create the branch and issue.
10. Create an isolated `skill-intake/<skill-id>-<timestamp>` branch in the selected
    repository.
11. Add the proposal under `proposals/<skill-id>/`.
12. Commit and push the proposal.
13. Open an issue linking the immutable commit and compare view, with a file inventory
    and maintainer checklist.

Use the AI host's authenticated GitHub tools. Do not request a pasted token.

## Boundaries

- Do not edit `skills.json`.
- Do not merge the branch.
- Do not mark the proposal approved.
- Do not install or execute the proposed skill.
- If the issue cannot be created after the branch is pushed, return the branch and
  commit so maintainers can recover the submission.
