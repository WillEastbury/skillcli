"""Generate README and SKILLS.md catalogue views from skills.json."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

from validate_manifest import validate_repository


START_MARKER = "<!-- BEGIN GENERATED SKILL CATALOG -->"
END_MARKER = "<!-- END GENERATED SKILL CATALOG -->"


def cell(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def requirements(skill: dict[str, Any]) -> str:
    capabilities = skill["capabilities"]
    values = []
    if capabilities["network"]:
        values.append("network: " + ", ".join(capabilities["network"]))
    if capabilities["filesystem"] != "none":
        values.append("filesystem: " + capabilities["filesystem"])
    if capabilities["shell"]:
        values.append("shell: " + ", ".join(capabilities["shell"]))
    if capabilities["mcpServers"]:
        values.append("MCP: " + ", ".join(capabilities["mcpServers"]))
    if capabilities["authentication"]:
        values.append("auth: " + ", ".join(capabilities["authentication"]))
    if skill["runtime"]["dependencies"]:
        values.append("dependencies: " + ", ".join(skill["runtime"]["dependencies"]))
    return "; ".join(values) or "none"


def table(skills: list[dict[str, Any]]) -> str:
    lines = [
        "| ID | Name | Version | Status | Requirements | Description |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for skill in sorted(skills, key=lambda item: item["id"]):
        lines.append(
            "| `{id}` | {name} | {version} | {status} | {requirements} | {description} |".format(
                id=cell(skill["id"]),
                name=cell(skill["name"]),
                version=cell(skill["version"]),
                status=cell(skill["status"]),
                requirements=cell(requirements(skill)),
                description=cell(skill["description"]),
            )
        )
    return "\n".join(lines)


def replace_catalog(readme: str, rendered: str) -> str:
    if readme.count(START_MARKER) != 1 or readme.count(END_MARKER) != 1:
        raise ValueError("README must contain exactly one generated catalogue marker pair")
    start, remainder = readme.split(START_MARKER, 1)
    _, end = remainder.split(END_MARKER, 1)
    return f"{start}{START_MARKER}\n{rendered}\n{END_MARKER}{end}"


def full_catalogue(manifest: dict[str, Any]) -> str:
    return "\n".join(
        [
            "# Complete Skill Catalogue",
            "",
            "Generated from `skills.json`. Do not edit manually.",
            "",
            f"**Registry version:** {manifest['manifestVersion']}",
            f"**Registered skills:** {len(manifest['skills'])}",
            "",
            table(manifest["skills"]),
            "",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    manifest, errors = validate_repository(root)
    if errors:
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    assert manifest is not None

    readme_path = root / "README.md"
    skills_path = root / "SKILLS.md"
    current_readme = readme_path.read_text(encoding="utf-8")
    core_ids = {"skill-zero", "skill-one", "skill-two"}
    core = [skill for skill in manifest["skills"] if skill["id"] in core_ids]
    expected_readme = replace_catalog(current_readme, table(core))
    expected_skills = full_catalogue(manifest)

    if args.check:
        stale = []
        if current_readme != expected_readme:
            stale.append("README.md")
        if not skills_path.is_file() or skills_path.read_text(encoding="utf-8") != expected_skills:
            stale.append("SKILLS.md")
        if stale:
            print("Generated files are stale: " + ", ".join(stale), file=sys.stderr)
            return 1
        print("Generated catalogue files are current")
        return 0

    readme_path.write_text(expected_readme, encoding="utf-8")
    skills_path.write_text(expected_skills, encoding="utf-8")
    print("Updated README.md and SKILLS.md from skills.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
