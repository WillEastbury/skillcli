"""Validate plugin marketplaces, checksums, and generated compatibility files."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from render_marketplace import build


SOURCE_ID = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
REPOSITORY = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")


def validate_sources(root: Path) -> list[str]:
    errors = []
    path = root / "skill-sources.json"
    if not path.exists():
        return []
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"skill-sources.json could not be read: {exc}"]
    sources = config.get("sources") if isinstance(config, dict) else None
    if not isinstance(sources, list) or not sources:
        return ["skill-sources.json sources must be a non-empty array"]
    seen = set()
    for index, source in enumerate(sources):
        label = f"sources[{index}]"
        if not isinstance(source, dict):
            errors.append(f"{label} must be an object")
            continue
        source_id = source.get("id")
        if not isinstance(source_id, str) or not SOURCE_ID.fullmatch(source_id):
            errors.append(f"{label}.id is invalid")
        elif source_id in seen:
            errors.append(f"duplicate source ID: {source_id}")
        else:
            seen.add(source_id)
        repository = source.get("repository")
        if not isinstance(repository, str) or not REPOSITORY.fullmatch(repository):
            errors.append(f"{label}.repository must use OWNER/REPO")
        if not isinstance(source.get("ref"), str) or not source["ref"].strip():
            errors.append(f"{label}.ref is invalid")
        if not isinstance(source.get("private"), bool):
            errors.append(f"{label}.private must be boolean")
        gh_user = source.get("ghUser")
        if gh_user is not None and (
            not isinstance(gh_user, str) or not gh_user.strip()
        ):
            errors.append(f"{label}.ghUser must be a non-empty string")
    return errors


def layout_warnings(root: Path) -> list[str]:
    """Flag a legacy proposals/ tree.

    This is a warning, not an error. Only plugins/ is ever read by the renderer, so
    anything left in proposals/ is inert rather than publishable, and an in-flight
    migration must not be blocked from validating the rest of the catalogue.
    """
    stale = root / "proposals"
    if stale.exists():
        return [
            "proposals/ is a legacy layout and is never published; migrate its content "
            "into plugins/<plugin-name>/ so merging a pull request publishes the skill"
        ]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--skip-sources", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    errors = [] if args.skip_sources else validate_sources(root)
    warnings = layout_warnings(root)
    try:
        expected = build(root, update=False)
        actual = json.loads((root / "skills.json").read_text(encoding="utf-8"))
        if actual != expected:
            errors.append("skills.json is stale; run python tools/render_marketplace.py")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        errors.append(str(exc))
    if errors:
        print("Marketplace validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    for warning in warnings:
        print(f"Warning: {warning}", file=sys.stderr)
    print(
        f"Marketplace valid: {expected['library']['name']}, "
        f"{len(expected['skills'])} plugin(s)"
    )
    for plugin in expected["skills"]:
        print(f"- {plugin['id']} {plugin['version']}: {plugin['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
