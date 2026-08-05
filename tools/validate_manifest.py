"""Validate plugin marketplaces, checksums, and generated compatibility files."""

from __future__ import annotations

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
    return errors


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    errors = validate_sources(root)
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
    print(
        f"Marketplace valid: {expected['library']['name']}, "
        f"{len(expected['skills'])} plugin(s)"
    )
    for plugin in expected["skills"]:
        print(f"- {plugin['id']} {plugin['version']}: {plugin['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
