"""Synchronise a plugin's version and review state across every file that declares it.

A plugin version lives in three places that must agree or generation fails:

- ``plugins/<name>/plugin.json``
- ``.github/plugin/marketplace.json``
- ``plugins/<name>/skills/<skill-id>/SKILL.md`` frontmatter

The review state lives in ``plugins/<name>/skillcli.json``. Maintainers set it to
``approved`` when accepting a submission pull request, so merging publishes the skill.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path, PurePosixPath

from render_marketplace import build, safe_path


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
STATES = ("pending", "approved", "rejected")


def load(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def dump(path: Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def set_frontmatter_version(path: Path, version: str) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "---":
        raise ValueError(f"{path} must begin with YAML frontmatter")
    end = lines.index("---", 1)
    for index in range(1, end):
        if lines[index].startswith("version:"):
            lines[index] = f'version: "{version}"'
            break
    else:
        raise ValueError(f"{path} frontmatter has no version field")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def apply(root: Path, plugin_name: str, version: str | None, state: str | None,
          reviewers: list[str] | None, reviewed_at: str | None) -> list[str]:
    marketplace_path = root / ".github" / "plugin" / "marketplace.json"
    marketplace = load(marketplace_path)
    entries = [
        entry
        for entry in marketplace.get("plugins", [])
        if isinstance(entry, dict) and entry.get("name") == plugin_name
    ]
    if not entries:
        raise ValueError(f"{plugin_name} is not registered in {marketplace_path.name}")
    entry = entries[0]
    plugin_root = root.joinpath(*PurePosixPath(safe_path(entry["source"])).parts)
    plugin_path = plugin_root / "plugin.json"
    metadata_path = plugin_root / "skillcli.json"
    plugin = load(plugin_path)
    metadata = load(metadata_path)
    skill_md = plugin_root.joinpath(
        *PurePosixPath(safe_path(metadata["skillRoot"])).parts,
        "SKILL.md",
    )
    changed = []
    if version is not None:
        if not SEMVER.fullmatch(version):
            raise ValueError(f"version must be MAJOR.MINOR.PATCH: {version}")
        plugin["version"] = version
        entry["version"] = version
        dump(plugin_path, plugin)
        dump(marketplace_path, marketplace)
        set_frontmatter_version(skill_md, version)
        changed.append(f"version -> {version}")
    if state is not None:
        if state not in STATES:
            raise ValueError(f"review state must be one of {', '.join(STATES)}")
        review = metadata.setdefault("review", {})
        review["state"] = state
        if reviewers:
            review["reviewedBy"] = reviewers
        review["reviewedAt"] = reviewed_at or dt.date.today().isoformat()
        dump(metadata_path, metadata)
        changed.append(f"review state -> {state}")
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--plugin", required=True, help="plugin name, e.g. skillcli-skill-one")
    parser.add_argument("--version", help="new MAJOR.MINOR.PATCH version")
    parser.add_argument("--state", choices=STATES, help="review state to record")
    parser.add_argument("--reviewer", action="append", dest="reviewers")
    parser.add_argument("--reviewed-at", help="ISO date; defaults to today")
    args = parser.parse_args()
    if args.version is None and args.state is None:
        parser.error("provide --version, --state, or both")
    root = args.root.resolve()
    try:
        changed = apply(
            root,
            args.plugin,
            args.version,
            args.state,
            args.reviewers,
            args.reviewed_at,
        )
        build(root, update=True)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"Metadata sync failed: {exc}", file=sys.stderr)
        return 1
    for entry in changed:
        print(f"{args.plugin}: {entry}")
    print("Updated plugin checksums and generated skills.json")
    print("Now run: python tools/render_readme.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
