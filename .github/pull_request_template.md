## Skill library change

Describe the catalogue, skill folder, taxonomy, tooling, or governance change.

## Validation

- [ ] `python tools/validate_manifest.py`
- [ ] `python tools/render_marketplace.py`
- [ ] `python tools/render_readme.py --check`
- [ ] `marketplace.json`, `plugin.json`, `skillcli.json`, and `SKILL.md` agree.
- [ ] Version synced with `python tools/sync_plugin_metadata.py --plugin <name> --version <x.y.z>`.
- [ ] New skills are submitted under `plugins/<plugin-name>/`, not `proposals/`.
- [ ] Maintainer set the review state before merge: `python tools/sync_plugin_metadata.py --plugin <name> --state approved --reviewer "<maintainer>"`.
- [ ] Capabilities, dependencies, MCP servers, authentication, source, and licence are complete.
- [ ] Changes to `skillcli.py`, `install.ps1`, `install.sh`, or `install-skill-zero.ps1` received maintainer review.
- [ ] No secrets, credentials, customer data, or personal data are included.
- [ ] New or changed skills have appropriate review evidence.
- [ ] Generated `skills.json`, `SKILLS.md`, and README views are current.
