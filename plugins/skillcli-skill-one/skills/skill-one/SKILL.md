---
name: "Skill One: GitHub Intake"
description: "Submits a proposed Markdown skill as a ready-to-merge plugin package on a review branch and opens a governed pull request."
version: "5.0.0"
skill-id: "skill-one"
---

# Skill One: GitHub Intake

## Purpose

Submit a proposed Markdown skill and any supporting files for review. The submission is
a complete plugin package placed in the catalogue's own `plugins/` tree on an isolated
review branch, and a pull request proposes it for merge.

The package is submitted with review state `pending`. Submission does not approve,
publish, merge, or install the skill. Maintainer approval and merge publish it.

## Procedure

1. Run `skillcli search --role skill-author --query "<proposed purpose>"`, or use native
   `copilot plugin marketplace browse <marketplace> --json`, to check for an existing
   approved plugin.
2. If the user selects an existing result, run
   `skillcli install --skill <owner>/<repo>/<skill-id>` and stop the submission flow.
3. Otherwise ask the user to confirm the target `OWNER/REPO` selected during Skill Two.
   If no repository was selected, stop and ask; never infer one.
4. Confirm the proposed lowercase kebab-case skill ID and plugin name.
5. Review the proposed plugin package and require `plugin.json`, `skillcli.json`, and
   `skills/<skill-id>/SKILL.md`. Accept `EVIDENCE.md` when Skill Two produced it.
6. Reject secrets, credentials, personal data, customer data, hidden files, symbolic
   links, binaries, and unrelated files.
7. Summarise the purpose, intended roles, categories, runtime, dependencies,
   capabilities, MCP servers, authentication, source, and licence.
8. Show the selected repository, branch, and pull request plan. The package is submitted
   to whichever repository the user selected, public or private, and never to both.
9. An explicit request to submit is approval to create the branch and pull request.
10. Create an isolated `skill-intake/<skill-id>-<timestamp>` branch in the selected
    repository.
11. Add the package under `plugins/<plugin-name>/` and register it in
    `.github/plugin/marketplace.json`.
12. Record the pending review state and regenerate the catalogue views so the branch is
    mergeable and continuous integration passes:

    ```text
    python tools/sync_plugin_metadata.py --plugin <plugin-name> --state pending
    python tools/render_readme.py
    python tools/validate_manifest.py
    ```

    Commit the regenerated `skills.json`, `SKILLS.md`, and `README.md` alongside the
    package. The catalogue table row appears in the pull request diff for review.
13. Commit and push the branch.
14. Open a pull request linking the immutable commit and compare view, with a file
    inventory and a maintainer checklist that includes flipping the review state:

    ```text
    python tools/sync_plugin_metadata.py --plugin <plugin-name> --state approved \
      --reviewer "<maintainer>"
    python tools/render_readme.py
    ```

    Merging the approved pull request publishes the skill immediately.

Use the AI host's authenticated GitHub tools. Do not request a pasted token.

## Boundaries

- Do not hand-edit `skills.json`, `SKILLS.md`, or the generated catalogue block in any
  `README.md`. Regenerate them with the repository tools so the row is derived from
  plugin metadata.
- Do not merge or approve the pull request.
- Do not set the review state to `approved`; submit as `pending` and leave approval to a
  maintainer.
- Do not install or execute the proposed skill.
- If the pull request cannot be created after the branch is pushed, return the branch and
  commit so maintainers can recover the submission.
