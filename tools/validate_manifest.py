"""Validate the governed skill catalogue and registered skill folders."""

from __future__ import annotations

import json
import hashlib
import os
import re
import stat
import sys
from datetime import date
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import urlparse


ID_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
SHA256_PATTERN = re.compile(r"^[a-f0-9]{64}$")
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


def is_reparse_point(path: Path) -> bool:
    try:
        attributes = getattr(os.lstat(path), "st_file_attributes", 0)
    except OSError:
        return False
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def windows_key(value: str, label: str, errors: list[str]) -> str | None:
    path = safe_relative(value, label, errors)
    if not path:
        return None
    keys = []
    for part in path.parts:
        if part.endswith((" ", ".")):
            errors.append(f"{label} has a trailing dot/space")
            return None
        if any(ord(character) < 32 or character in WINDOWS_INVALID for character in part):
            errors.append(f"{label} has Windows-invalid characters")
            return None
        if part.split(".", 1)[0].casefold() in WINDOWS_RESERVED:
            errors.append(f"{label} uses a Windows reserved name")
            return None
        if WINDOWS_SHORT_NAME.fullmatch(part):
            errors.append(f"{label} uses a Windows 8.3-style name")
            return None
        keys.append(part.casefold())
    return "/".join(keys)


def canonical_bytes(path: Path) -> bytes:
    content = path.read_bytes()
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        return content
    return text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


def safe_relative(value: Any, label: str, errors: list[str]) -> PurePosixPath | None:
    if not isinstance(value, str) or not value or "\\" in value:
        errors.append(f"{label} must be a normalized relative path")
        return None
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or path.as_posix() != value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        errors.append(f"{label} is unsafe: {value!r}")
        return None
    return path


def string_list(
    value: Any,
    label: str,
    errors: list[str],
    *,
    identifiers: bool = False,
) -> list[str]:
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item.strip() for item in value
    ):
        errors.append(f"{label} must be an array of non-empty strings")
        return []
    if len(value) != len(set(value)):
        errors.append(f"{label} must not contain duplicates")
    if identifiers and not all(ID_PATTERN.fullmatch(item) for item in value):
        errors.append(f"{label} must contain lowercase kebab-case identifiers")
    return value


def frontmatter(path: Path, errors: list[str]) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        errors.append(f"{path} could not be read as UTF-8: {exc}")
        return {}
    if not lines or lines[0] != "---":
        errors.append(f"{path} must begin with YAML frontmatter")
        return {}
    try:
        end = lines.index("---", 1)
    except ValueError:
        errors.append(f"{path} frontmatter is not closed")
        return {}
    values: dict[str, str] = {}
    for line in lines[1:end]:
        if not line.strip():
            continue
        if ":" not in line:
            errors.append(f"{path} has invalid frontmatter line: {line}")
            continue
        key, value = line.split(":", 1)
        key = key.strip()
        raw_value = value.strip()
        if not (
            len(raw_value) >= 2
            and raw_value.startswith('"')
            and raw_value.endswith('"')
        ):
            errors.append(
                f"{path} frontmatter value for {key!r} must be double-quoted"
            )
            continue
        try:
            parsed_value = json.loads(raw_value)
        except json.JSONDecodeError:
            errors.append(f"{path} has invalid quoted frontmatter for {key!r}")
            continue
        if not isinstance(parsed_value, str):
            errors.append(f"{path} frontmatter {key!r} must be a string")
            continue
        values[key] = parsed_value
    return values


def validate_repository(root: Path) -> tuple[dict[str, Any] | None, list[str]]:
    root = root.resolve()
    errors: list[str] = []
    try:
        manifest = json.loads((root / "skills.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, [f"skills.json could not be read: {exc}"]
    if not isinstance(manifest, dict):
        return None, ["skills.json must contain a JSON object"]

    version = manifest.get("manifestVersion")
    if not isinstance(version, str) or not VERSION_PATTERN.fullmatch(version):
        errors.append("manifestVersion must use X.Y.Z")

    library = manifest.get("library")
    if not isinstance(library, dict):
        errors.append("library must be an object")
        return manifest, errors

    repository = library.get("repository")
    parsed = urlparse(repository) if isinstance(repository, str) else None
    if not parsed or parsed.scheme != "https" or not parsed.netloc:
        errors.append("library.repository must be an HTTPS URL")
    if library.get("updatePolicy") != "notify-only":
        errors.append("library.updatePolicy must be notify-only")
    namespace = library.get("namespace")
    if not isinstance(namespace, str) or not re.fullmatch(
        r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+",
        namespace,
    ):
        errors.append("library.namespace must use OWNER/REPO")

    taxonomy = manifest.get("taxonomy")
    if not isinstance(taxonomy, dict):
        errors.append("taxonomy must be an object")
        return manifest, errors
    roles = taxonomy.get("roles")
    tasks = taxonomy.get("taskCategories")
    role_ids: set[str] = set()
    task_ids: set[str] = set()
    for records, target, label in (
        (roles, role_ids, "taxonomy.roles"),
        (tasks, task_ids, "taxonomy.taskCategories"),
    ):
        if not isinstance(records, list) or not records:
            errors.append(f"{label} must be a non-empty array")
            continue
        for index, record in enumerate(records):
            if not isinstance(record, dict):
                errors.append(f"{label}[{index}] must be an object")
                continue
            identifier = record.get("id")
            name = record.get("name")
            if not isinstance(identifier, str) or not ID_PATTERN.fullmatch(identifier):
                errors.append(f"{label}[{index}].id is invalid")
            elif identifier in target:
                errors.append(f"duplicate taxonomy ID: {identifier}")
            else:
                target.add(identifier)
            if not isinstance(name, str) or not name.strip():
                errors.append(f"{label}[{index}].name is invalid")
    max_results = taxonomy.get("maxResults")
    if not isinstance(max_results, int) or not 1 <= max_results <= 10:
        errors.append("taxonomy.maxResults must be between 1 and 10")

    skills = manifest.get("skills")
    if not isinstance(skills, list) or not skills:
        errors.append("skills must be a non-empty array")
        return manifest, errors

    seen: set[str] = set()
    for index, skill in enumerate(skills):
        label = f"skills[{index}]"
        if not isinstance(skill, dict):
            errors.append(f"{label} must be an object")
            continue
        skill_id = skill.get("id")
        if not isinstance(skill_id, str) or not ID_PATTERN.fullmatch(skill_id):
            errors.append(f"{label}.id is invalid")
            continue
        if skill_id in seen:
            errors.append(f"duplicate skill ID: {skill_id}")
        seen.add(skill_id)

        skill_version = skill.get("version")
        if not isinstance(skill_version, str) or not VERSION_PATTERN.fullmatch(skill_version):
            errors.append(f"{label}.version must use X.Y.Z")
        if skill.get("status") not in {
            "discovered",
            "review",
            "approved",
            "deprecated",
            "blocked",
        }:
            errors.append(f"{label}.status is invalid")

        assigned_roles = string_list(skill.get("roles"), f"{label}.roles", errors, identifiers=True)
        assigned_tasks = string_list(
            skill.get("taskCategories"),
            f"{label}.taskCategories",
            errors,
            identifiers=True,
        )
        string_list(skill.get("keywords"), f"{label}.keywords", errors, identifiers=True)
        unknown_roles = sorted(set(assigned_roles) - role_ids)
        unknown_tasks = sorted(set(assigned_tasks) - task_ids)
        if unknown_roles:
            errors.append(f"{label}.roles contains unknown IDs: {', '.join(unknown_roles)}")
        if unknown_tasks:
            errors.append(
                f"{label}.taskCategories contains unknown IDs: {', '.join(unknown_tasks)}"
            )

        path_value = skill.get("path")
        expected_path = f"skills/{skill_id}"
        if path_value != expected_path:
            errors.append(f"{label}.path must be {expected_path}")
        relative = safe_relative(path_value, f"{label}.path", errors)
        if not relative:
            continue
        folder = root.joinpath(*relative.parts)
        try:
            folder.resolve().relative_to(root)
        except ValueError:
            errors.append(f"{label}.path escapes the repository")
            continue
        if not folder.is_dir() or folder.is_symlink():
            errors.append(f"{label}.path must be a real directory")
            continue
        symbolic = [
            item.relative_to(folder).as_posix()
            for item in folder.rglob("*")
            if item.is_symlink() or is_reparse_point(item)
        ]
        if symbolic:
            errors.append(
                f"{label}.path contains links/reparse points: {', '.join(symbolic)}"
            )

        entrypoint = skill.get("entrypoint")
        if entrypoint != "SKILL.md":
            errors.append(f"{label}.entrypoint must be SKILL.md")
            continue
        entrypoint_path = folder / "SKILL.md"
        if not entrypoint_path.is_file():
            errors.append(f"{label} is missing SKILL.md")
            continue
        metadata = frontmatter(entrypoint_path, errors)
        expected_frontmatter = {
            "name": skill.get("name"),
            "description": skill.get("description"),
            "version": skill_version,
            "skill-id": skill_id,
        }
        for key, expected in expected_frontmatter.items():
            if metadata.get(key) != expected:
                errors.append(
                    f"{label} frontmatter {key!r} does not match skills.json"
                )

        files = skill.get("files")
        if not isinstance(files, list) or not files:
            errors.append(f"{label}.files must be a non-empty array")
        else:
            declared: dict[str, str] = {}
            for file_index, record in enumerate(files):
                file_label = f"{label}.files[{file_index}]"
                if not isinstance(record, dict):
                    errors.append(f"{file_label} must be an object")
                    continue
                path_value = record.get("path")
                key = (
                    windows_key(path_value, f"{file_label}.path", errors)
                    if isinstance(path_value, str)
                    else None
                )
                if key:
                    if key in declared:
                        errors.append(
                            f"{file_label}.path collides with {declared[key]} on Windows"
                        )
                    else:
                        declared[key] = path_value
                digest = record.get("sha256")
                if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
                    errors.append(f"{file_label}.sha256 is invalid")
                if isinstance(path_value, str):
                    relative_file = safe_relative(
                        path_value,
                        f"{file_label}.path",
                        errors,
                    )
                    if relative_file:
                        file_path = folder.joinpath(*relative_file.parts)
                        try:
                            file_path.resolve().relative_to(folder.resolve())
                        except ValueError:
                            errors.append(f"{file_label}.path escapes the skill folder")
                            continue
                        if not file_path.is_file():
                            errors.append(f"{file_label}.path is missing")
                        elif (
                            isinstance(digest, str)
                            and SHA256_PATTERN.fullmatch(digest)
                            and hashlib.sha256(canonical_bytes(file_path)).hexdigest()
                            != digest
                        ):
                            errors.append(f"{file_label}.sha256 does not match the file")
            actual: dict[str, str] = {}
            for item in folder.rglob("*"):
                if not item.is_file():
                    continue
                relative_actual = item.relative_to(folder).as_posix()
                key = windows_key(
                    relative_actual,
                    f"{label} file {relative_actual!r}",
                    errors,
                )
                if key:
                    if key in actual:
                        errors.append(
                            f"{label} file {relative_actual!r} collides with "
                            f"{actual[key]!r} on Windows"
                        )
                    else:
                        actual[key] = relative_actual
            undeclared = sorted(set(actual) - set(declared))
            stale = sorted(set(declared) - set(actual))
            if undeclared:
                errors.append(
                    f"{label} has undeclared files: "
                    + ", ".join(actual[key] for key in undeclared)
                )
            if stale:
                errors.append(
                    f"{label} declares absent files: "
                    + ", ".join(declared[key] for key in stale)
                )

        review = skill.get("review")
        if not isinstance(review, dict):
            errors.append(f"{label}.review must be an object")
        else:
            state = review.get("state")
            reviewers = string_list(
                review.get("reviewedBy"),
                f"{label}.review.reviewedBy",
                errors,
            )
            reviewed_at = review.get("reviewedAt")
            if reviewed_at is not None:
                try:
                    if not isinstance(reviewed_at, str):
                        raise ValueError
                    date.fromisoformat(reviewed_at)
                except ValueError:
                    errors.append(f"{label}.review.reviewedAt is invalid")
            if skill.get("status") == "approved" and (
                state != "approved" or not reviewers or reviewed_at is None
            ):
                errors.append(f"{label} is approved without a complete review")

    browser_id = library.get("browserSkillId")
    if browser_id is not None and browser_id not in seen:
        errors.append("library.browserSkillId is not registered")
    elif browser_id is not None:
        browser = next(item for item in skills if item.get("id") == browser_id)
        if browser.get("status") != "approved" or browser.get("review", {}).get(
            "state"
        ) != "approved":
            errors.append("library browser skill must be approved")

    return manifest, errors


def validate_sources(root: Path) -> list[str]:
    errors: list[str] = []
    path = root / "skill-sources.json"
    if not path.is_file():
        return ["skill-sources.json is missing"]
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"skill-sources.json could not be read: {exc}"]
    if not isinstance(config, dict):
        return ["skill-sources.json must contain an object"]
    sources = config.get("sources")
    if not isinstance(sources, list) or not sources:
        errors.append("skill-sources.json sources must be a non-empty array")
        return errors
    seen: set[str] = set()
    for index, source in enumerate(sources):
        label = f"skill-sources.json sources[{index}]"
        if not isinstance(source, dict):
            errors.append(f"{label} must be an object")
            continue
        source_id = source.get("id")
        if not isinstance(source_id, str) or not ID_PATTERN.fullmatch(source_id):
            errors.append(f"{label}.id is invalid")
        elif source_id in seen:
            errors.append(f"duplicate source ID: {source_id}")
        else:
            seen.add(source_id)
        repository = source.get("repository")
        if not isinstance(repository, str) or not re.fullmatch(
            r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+",
            repository,
        ):
            errors.append(f"{label}.repository must use OWNER/REPO")
        if not isinstance(source.get("ref"), str) or not source["ref"].strip():
            errors.append(f"{label}.ref is invalid")
        if not isinstance(source.get("private"), bool):
            errors.append(f"{label}.private must be boolean")
    return errors


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    manifest, errors = validate_repository(root)
    errors.extend(validate_sources(root))
    if errors:
        print("Skill catalogue validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    assert manifest is not None
    print(
        f"Catalogue valid: {manifest['library']['name']} "
        f"v{manifest['manifestVersion']}, {len(manifest['skills'])} skill(s)"
    )
    for skill in manifest["skills"]:
        print(f"- {skill['id']} {skill['version']}: {skill['status']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
