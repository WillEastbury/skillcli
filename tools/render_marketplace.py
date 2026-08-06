"""Update plugin checksums and generate the compatibility skills.json index."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path, PurePosixPath
from typing import Any


WINDOWS_INVALID = set('<>:"|?*')
WINDOWS_RESERVED = {
    "con",
    "prn",
    "aux",
    "nul",
    *(f"com{index}" for index in range(1, 10)),
    *(f"lpt{index}" for index in range(1, 10)),
}
WINDOWS_SHORT_NAME = re.compile(r"^[^.]{1,6}~[0-9]+(?:\.[^.]{0,3})?$", re.IGNORECASE)


def safe_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        not value
        or "\\" in value
        or path.is_absolute()
        or path.as_posix() != value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise ValueError(f"unsafe relative path: {value!r}")
    return path


def windows_key(value: str) -> str:
    path = safe_path(value)
    keys = []
    for part in path.parts:
        if part.endswith((" ", ".")):
            raise ValueError(f"Windows trailing dot/space: {value}")
        if any(ord(character) < 32 or character in WINDOWS_INVALID for character in part):
            raise ValueError(f"Windows-invalid path: {value}")
        if part.split(".", 1)[0].casefold() in WINDOWS_RESERVED:
            raise ValueError(f"Windows reserved path: {value}")
        if WINDOWS_SHORT_NAME.fullmatch(part):
            raise ValueError(f"Windows 8.3-style path: {value}")
        keys.append(part.casefold())
    return "/".join(keys)


def is_reparse_point(path: Path) -> bool:
    try:
        attributes = getattr(os.lstat(path), "st_file_attributes", 0)
    except OSError:
        return False
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def canonical_bytes(path: Path) -> bytes:
    content = path.read_bytes()
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        return content
    return text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


def frontmatter(path: Path) -> dict[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "---":
        raise ValueError(f"{path} must begin with YAML frontmatter")
    try:
        end = lines.index("---", 1)
    except ValueError as exc:
        raise ValueError(f"{path} frontmatter is not closed") from exc
    values = {}
    for line in lines[1:end]:
        if not line.strip():
            continue
        if ":" not in line:
            raise ValueError(f"invalid frontmatter line in {path}: {line}")
        key, raw = line.split(":", 1)
        raw = raw.strip()
        if not (raw.startswith('"') and raw.endswith('"')):
            raise ValueError(f"{path} frontmatter {key.strip()} must be double-quoted")
        value = json.loads(raw)
        if not isinstance(value, str):
            raise ValueError(f"{path} frontmatter {key.strip()} must be a string")
        values[key.strip()] = value
    return values


def json_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def plugin_records(plugin_root: Path, metadata: dict[str, Any]) -> list[dict[str, str]]:
    skill_root = safe_path(metadata["skillRoot"]).as_posix()
    skill_folder = plugin_root.joinpath(*PurePosixPath(skill_root).parts)
    if not skill_folder.is_dir():
        raise ValueError(f"skillRoot is missing: {skill_root}")
    selected: list[tuple[str, str]] = []
    for path in sorted(skill_folder.rglob("*")):
        if path.is_dir():
            continue
        relative = path.relative_to(plugin_root).as_posix()
        selected.append((relative, "skill"))
    for value in metadata.get("toolFiles", []):
        relative = safe_path(value).as_posix()
        selected.append((relative, "tool"))
    allowed = {relative for relative, _ in selected} | {"plugin.json", "skillcli.json"}
    actual = {
        path.relative_to(plugin_root).as_posix()
        for path in plugin_root.rglob("*")
        if path.is_file()
    }
    undeclared = sorted(actual - allowed)
    if undeclared:
        raise ValueError(
            "plugin contains undeclared files: " + ", ".join(undeclared)
        )
    seen: dict[str, str] = {}
    records = []
    for relative, target in selected:
        key = windows_key(relative)
        if key in seen:
            raise ValueError(f"Windows path collision: {relative} and {seen[key]}")
        seen[key] = relative
        path = plugin_root.joinpath(*PurePosixPath(relative).parts)
        if not path.is_file() or path.is_symlink() or is_reparse_point(path):
            raise ValueError(f"plugin file is missing or linked: {relative}")
        records.append(
            {
                "path": relative,
                "sha256": hashlib.sha256(canonical_bytes(path)).hexdigest(),
                "target": target,
            }
        )
    return sorted(records, key=lambda item: item["path"])


def build(root: Path, update: bool) -> dict[str, Any]:
    catalogue = json_object(root / "catalogue.json")
    marketplace = json_object(root / ".github" / "plugin" / "marketplace.json")
    entries = marketplace.get("plugins")
    if not isinstance(entries, list) or not entries:
        raise ValueError("marketplace.json must contain plugins")
    generated_skills = []
    listed_plugin_paths = set()
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("source"), str):
            raise ValueError("only local string plugin sources are supported")
        plugin_path = safe_path(entry["source"]).as_posix()
        listed_plugin_paths.add(plugin_path)
        plugin_root = root.joinpath(*PurePosixPath(plugin_path).parts)
        plugin_manifest_path = plugin_root / "plugin.json"
        metadata_path = plugin_root / "skillcli.json"
        plugin = json_object(plugin_manifest_path)
        metadata = json_object(metadata_path)
        if plugin.get("name") != entry.get("name"):
            raise ValueError(f"marketplace/plugin name mismatch: {plugin_path}")
        if plugin.get("version") != entry.get("version"):
            raise ValueError(f"marketplace/plugin version mismatch: {plugin_path}")
        records = plugin_records(plugin_root, metadata)
        if update:
            metadata["files"] = records
            metadata_path.write_text(
                json.dumps(metadata, indent=2, ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
        elif metadata.get("files") != records:
            raise ValueError(f"plugin checksums are stale: {plugin_path}")
        skill_root = safe_path(metadata["skillRoot"]).as_posix()
        skill_md = plugin_root.joinpath(
            *PurePosixPath(skill_root).parts,
            "SKILL.md",
        )
        fm = frontmatter(skill_md)
        if fm.get("version") != plugin.get("version"):
            raise ValueError(f"SKILL.md/plugin version mismatch: {plugin_path}")
        skill_files = [
            {
                "path": record["path"][len(skill_root) + 1 :],
                "sha256": record["sha256"],
            }
            for record in records
            if record["target"] == "skill"
        ]
        generated_skills.append(
            {
                "id": plugin["name"],
                "name": fm["name"],
                "version": plugin["version"],
                "description": fm["description"],
                "status": "approved" if metadata["review"]["state"] == "approved" else "review",
                "path": f"{plugin_path}/{skill_root}",
                "entrypoint": "SKILL.md",
                "roles": metadata["roles"],
                "taskCategories": metadata["taskCategories"],
                "keywords": plugin.get("keywords", []),
                "runtime": metadata["runtime"],
                "capabilities": metadata["capabilities"],
                "source": {
                    "type": "internal",
                    "repository": catalogue["library"]["repository"],
                    "license": plugin.get("license", "Unspecified"),
                },
                "review": metadata["review"],
                "files": skill_files,
            }
        )
    for metadata_path in sorted((root / "plugins").glob("*/skillcli.json")):
        plugin_root = metadata_path.parent
        relative_plugin = plugin_root.relative_to(root).as_posix()
        if relative_plugin in listed_plugin_paths:
            continue
        metadata = json_object(metadata_path)
        records = plugin_records(plugin_root, metadata)
        if update:
            metadata["files"] = records
            metadata_path.write_text(
                json.dumps(metadata, indent=2, ensure_ascii=True) + "\n",
                encoding="utf-8",
            )
        elif metadata.get("files") != records:
            raise ValueError(f"unlisted plugin checksums are stale: {relative_plugin}")
    library = dict(catalogue["library"])
    library.pop("readmePlugins", None)
    browser_plugin = library.pop("browserPlugin", None)
    if browser_plugin:
        library["browserSkillId"] = browser_plugin
    manifest = {
        "$schema": "schemas/skills.schema.json",
        "manifestVersion": "5.0.0",
        "library": library,
        "taxonomy": catalogue["taxonomy"],
        "skills": generated_skills,
    }
    if update:
        (root / "skills.json").write_text(
            json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
            encoding="utf-8",
        )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
    )
    args = parser.parse_args()
    root = args.root.resolve()
    build(root, update=True)
    print("Updated plugin checksums and generated skills.json")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Marketplace generation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
