"""Managed multi-source skill catalogue CLI."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import quote


QUALIFIED_ID_PATTERN = re.compile(
    r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/[a-z0-9]+(?:-[a-z0-9]+)*$"
)
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
DEFAULT_CONFIG = {
    "sources": [
        {
            "id": "public",
            "repository": "WillEastbury/skillcli",
            "ref": "main",
            "private": False,
        }
    ],
}


def safe_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        not value
        or "\\" in value
        or path.is_absolute()
        or path.as_posix() != value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise ValueError(f"unsafe catalogue path: {value!r}")
    return path


def windows_key(value: str) -> str:
    path = safe_path(value)
    keys = []
    for part in path.parts:
        if part.endswith((" ", ".")):
            raise ValueError(f"Windows path component has a trailing dot/space: {value}")
        if any(ord(character) < 32 or character in WINDOWS_INVALID for character in part):
            raise ValueError(f"Windows path component has invalid characters: {value}")
        if part.split(".", 1)[0].casefold() in WINDOWS_RESERVED:
            raise ValueError(f"Windows reserved path component: {value}")
        if WINDOWS_SHORT_NAME.fullmatch(part):
            raise ValueError(f"Windows 8.3-style path component: {value}")
        keys.append(part.casefold())
    return "/".join(keys)


def is_reparse_point(path: Path) -> bool:
    try:
        attributes = getattr(os.lstat(path), "st_file_attributes", 0)
    except OSError:
        return False
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def gh(arguments: list[str]) -> bytes:
    result = subprocess.run(
        ["gh", *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(message or f"gh exited with code {result.returncode}")
    return result.stdout


def public_json(endpoint: str) -> dict[str, Any]:
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "skillcli",
    }
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(
        f"https://api.github.com/{endpoint}",
        headers=headers,
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            value = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"GitHub HTTP {exc.code} for {endpoint}") from exc
    if not isinstance(value, dict):
        raise RuntimeError(f"GitHub returned invalid JSON for {endpoint}")
    return value


def public_bytes(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "skillcli"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"GitHub HTTP {exc.code} for {url}") from exc


def load_config() -> dict[str, Any]:
    candidates = []
    configured = os.environ.get("SKILLCLI_CONFIG")
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.append(Path(__file__).resolve().with_name("sources.json"))
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        candidates.append(
            Path(local_app_data) / "skillcli" / "sources.json"
        )
    for path in candidates:
        if path.is_file():
            value = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(value, dict):
                raise ValueError(f"source configuration must be an object: {path}")
            return value
    return DEFAULT_CONFIG


@dataclass(frozen=True)
class SourceConfig:
    id: str
    repository: str
    ref: str
    private: bool


class Source:
    def __init__(self, config: SourceConfig) -> None:
        self.config = config
        self.commit: str | None = None
        self._manifest_bytes: bytes | None = None
        self._manifest: dict[str, Any] | None = None

    def api_json(self, endpoint: str) -> dict[str, Any]:
        if self.config.private:
            value = json.loads(gh(["api", endpoint]).decode("utf-8"))
            if not isinstance(value, dict):
                raise RuntimeError(f"GitHub returned invalid JSON for {endpoint}")
            return value
        return public_json(endpoint)

    def resolve(self) -> str:
        if self.commit:
            return self.commit
        response = self.api_json(
            f"repos/{self.config.repository}/commits/{quote(self.config.ref, safe='')}"
        )
        self.commit = response["sha"]
        return self.commit

    def read(self, path: str) -> bytes:
        relative = safe_path(path).as_posix()
        commit = self.resolve()
        if self.config.private:
            endpoint = (
                f"repos/{self.config.repository}/contents/{quote(relative, safe='/')}"
                f"?ref={quote(commit, safe='')}"
            )
            return gh(
                [
                    "api",
                    "-H",
                    "Accept: application/vnd.github.raw+json",
                    endpoint,
                ]
            )
        return public_bytes(
            f"https://raw.githubusercontent.com/"
            f"{self.config.repository}/{commit}/{quote(relative, safe='/')}"
        )

    def manifest_bytes(self) -> bytes:
        if self._manifest_bytes is None:
            self._manifest_bytes = self.read("skills.json")
        return self._manifest_bytes

    def manifest(self) -> dict[str, Any]:
        if self._manifest is None:
            value = json.loads(self.manifest_bytes().decode("utf-8"))
            if not isinstance(value, dict) or not isinstance(value.get("skills"), list):
                raise ValueError(
                    f"{self.config.id} skills.json is not a valid catalogue"
                )
            self._manifest = value
        return self._manifest

@dataclass(frozen=True)
class Skill:
    source: Source
    metadata: dict[str, Any]


class Catalogues:
    def __init__(self) -> None:
        config = load_config()
        source_values = config.get("sources")
        if not isinstance(source_values, list) or not source_values:
            raise ValueError("source configuration must contain sources")
        self.sources = [
            Source(
                SourceConfig(
                    id=value["id"],
                    repository=value["repository"],
                    ref=value.get("ref", "main"),
                    private=bool(value.get("private", False)),
                )
            )
            for value in source_values
        ]
        self.warnings: list[str] = []
        self.skills: dict[str, Skill] = {}
        self._load()

    def _load(self) -> None:
        available = 0
        for source in self.sources:
            try:
                manifest = source.manifest()
            except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
                self.warnings.append(f"{source.config.id}: {exc}")
                continue
            available += 1
            namespace = manifest.get("library", {}).get("namespace")
            if namespace != source.config.repository:
                self.warnings.append(
                    f"{source.config.id}: namespace {namespace!r} does not match "
                    f"repository {source.config.repository!r}"
                )
                continue
            for metadata in manifest["skills"]:
                skill_id = metadata.get("id")
                if not isinstance(skill_id, str):
                    continue
                qualified_id = f"{namespace}/{skill_id}"
                if qualified_id in self.skills:
                    raise ValueError(f"duplicate qualified skill ID: {qualified_id}")
                self.skills[qualified_id] = Skill(source, metadata)
        if not available:
            raise RuntimeError("no configured skill catalogue could be loaded")

    def skill(self, qualified_id: str) -> Skill:
        try:
            return self.skills[qualified_id]
        except KeyError as exc:
            raise ValueError(f"unknown skill: {qualified_id}") from exc

    def snapshot(self) -> bytes:
        value = {
            "sources": [
                {
                    "id": source.config.id,
                    "repository": source.config.repository,
                    "ref": source.config.ref,
                }
                for source in self.sources
            ],
            "skills": [
                {
                    **skill.metadata,
                    "qualifiedId": qualified_id,
                    "catalogueSource": skill.source.config.id,
                }
                for qualified_id, skill in sorted(self.skills.items())
            ],
        }
        return (json.dumps(value, indent=2) + "\n").encode("utf-8")


def destinations() -> dict[str, Path]:
    override = os.environ.get("SKILLCLI_DESTINATIONS")
    if override:
        value = json.loads(override)
        if not isinstance(value, dict):
            raise ValueError("SKILLCLI_DESTINATIONS must be a JSON object")
        return {name: Path(path).expanduser().resolve() for name, path in value.items()}
    home = Path.home()
    result: dict[str, Path] = {}
    if (home / ".copilot").exists():
        result["copilot-cli"] = home / ".copilot" / "skills"
    if (home / ".scout").exists():
        result["scout"] = home / ".scout" / "m-skills"
    for variable in ("OneDriveCommercial", "OneDrive", "OneDriveConsumer"):
        value = os.environ.get(variable)
        if value and (Path(value) / "Documents" / "Cowork").exists():
            result["copilot-cowork"] = Path(value) / "Documents" / "Cowork" / "Skills"
            break
    if not result:
        raise ValueError("no Copilot CLI, Scout, or Co-Work skill folders were detected")
    return result


def frontmatter(path: Path) -> dict[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "---":
        return {}
    try:
        end = lines.index("---", 1)
    except ValueError:
        return {}
    result = {}
    for line in lines[1:end]:
        if ":" in line:
            key, value = line.split(":", 1)
            result[key.strip()] = value.strip().strip("\"'")
    return result


def terms(value: str) -> set[str]:
    return {item for item in re.findall(r"[a-z0-9]+", value.lower()) if len(item) > 1}


def requirements(skill: dict[str, Any]) -> str:
    values = []
    capabilities = skill["capabilities"]
    if capabilities["network"]:
        values.append("network")
    if capabilities["filesystem"] != "none":
        values.append(capabilities["filesystem"] + " files")
    if capabilities["shell"]:
        values.append("shell")
    if capabilities["mcpServers"]:
        values.append("MCP")
    if capabilities["authentication"]:
        values.append("auth")
    if skill["runtime"]["dependencies"]:
        values.append("dependencies")
    return ", ".join(values) or "none"


def table(headers: list[str], rows: list[list[Any]]) -> str:
    widths = [len(header) for header in headers]
    text_rows = [[str(value) for value in row] for row in rows]
    for row in text_rows:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))
    lines = [
        "  ".join(header.ljust(widths[index]) for index, header in enumerate(headers)),
        "  ".join("-" * width for width in widths),
    ]
    lines.extend(
        "  ".join(value.ljust(widths[index]) for index, value in enumerate(row))
        for row in text_rows
    )
    return "\n".join(lines)


def show_warnings(catalogues: Catalogues) -> None:
    for warning in catalogues.warnings:
        print(f"warning: {warning}", file=sys.stderr)


def search(catalogues: Catalogues, role: str, query: str) -> None:
    query_terms = terms(query)
    matches = []
    for qualified_id, skill in catalogues.skills.items():
        metadata = skill.metadata
        if metadata["status"] != "approved" or metadata["review"]["state"] != "approved":
            continue
        searchable = terms(
            " ".join(
                [
                    metadata["id"],
                    metadata["name"],
                    metadata["description"],
                    *metadata["keywords"],
                    *metadata["taskCategories"],
                ]
            )
        )
        matched = query_terms & searchable
        if query_terms and not matched:
            continue
        score = len(matched) * 4 + (3 if role in metadata["roles"] else 0)
        reason = ", ".join(sorted(matched)) or "role match"
        matches.append((score, qualified_id, skill, reason))
    matches.sort(key=lambda item: (-item[0], item[1]))
    rows = [
        [
            rank,
            qualified_id,
            skill.metadata["name"],
            skill.metadata["version"],
            skill.source.config.id,
            reason,
            requirements(skill.metadata),
        ]
        for rank, (_, qualified_id, skill, reason) in enumerate(matches[:10], start=1)
    ]
    print(
        table(
            ["#", "Skill ID", "Name", "Version", "Source", "Why", "Requirements"],
            rows,
        )
    )
    show_warnings(catalogues)


def reject_symlinks(path: Path) -> None:
    current = path
    while current != current.parent:
        if current.exists() and (current.is_symlink() or is_reparse_point(current)):
            raise ValueError(f"destination contains a link/reparse point: {current}")
        current = current.parent
    if path.exists():
        for child in path.rglob("*"):
            if child.is_symlink() or is_reparse_point(child):
                raise ValueError(f"destination contains a link/reparse point: {child}")


def remote_content(skill: Skill) -> dict[str, bytes]:
    prefix = safe_path(skill.metadata["path"]).as_posix() + "/"
    records = skill.metadata.get("files")
    if not isinstance(records, list) or not records:
        raise ValueError(f"skill has no declared files: {skill.metadata['id']}")
    content: dict[str, bytes] = {}
    keys: set[str] = set()
    for record in records:
        if not isinstance(record, dict):
            raise ValueError(f"skill has an invalid file record: {skill.metadata['id']}")
        relative = record.get("path")
        digest = record.get("sha256")
        if not isinstance(relative, str) or not isinstance(digest, str):
            raise ValueError(f"skill has an invalid file record: {skill.metadata['id']}")
        key = windows_key(relative)
        if key in keys:
            raise ValueError(f"skill has a Windows path collision: {relative}")
        keys.add(key)
        if not SHA256_PATTERN.fullmatch(digest):
            raise ValueError(f"skill has an invalid checksum: {relative}")
        value = skill.source.read(prefix + safe_path(relative).as_posix())
        actual = hashlib.sha256(value).hexdigest()
        if actual != digest:
            raise ValueError(
                f"checksum mismatch for {skill.source.config.repository}/{relative}"
            )
        content[relative] = value
    if skill.metadata["entrypoint"] not in content:
        raise ValueError(f"skill entrypoint is not declared: {skill.metadata['id']}")
    return content


def qualified_folder(qualified_id: str) -> str:
    if not QUALIFIED_ID_PATTERN.fullmatch(qualified_id):
        raise ValueError("skill ID must use OWNER/REPO/skill-id")
    return qualified_id.replace("/", "!")


def replace_folder(
    root: Path,
    qualified_id: str,
    skill: Skill,
    content: dict[str, bytes],
    snapshot: bytes,
    host: str,
    allow_existing: bool,
) -> tuple[str, str]:
    target = root / qualified_folder(qualified_id)
    reject_symlinks(target)
    existed = target.exists()
    if existed and not allow_existing:
        return str(target), "already installed"
    if existed:
        receipt_path = target / ".skillcli.json"
        if receipt_path.is_file():
            try:
                receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                return str(target), "invalid source metadata"
            if (
                receipt.get("qualifiedId") != qualified_id
                or receipt.get("source", {}).get("repository")
                != skill.source.config.repository
            ):
                return str(target), "source namespace mismatch"
            for record in receipt.get("files", []):
                relative = record.get("path")
                digest = record.get("sha256")
                if not isinstance(relative, str) or not isinstance(digest, str):
                    return str(target), "invalid source metadata"
                local_file = target.joinpath(*safe_path(relative).parts)
                if (
                    not local_file.is_file()
                    or hashlib.sha256(local_file.read_bytes()).hexdigest() != digest
                ):
                    return str(target), "local files modified"
        else:
            return str(target), "missing source metadata"
        existing = {
            path.relative_to(target).as_posix()
            for path in target.rglob("*")
            if path.is_file()
        }
        unexpected = sorted(
            existing - set(content) - {"catalogue.json", ".skillcli.json"}
        )
        if unexpected:
            return str(target), "local files present"
    root.mkdir(parents=True, exist_ok=True)
    transaction = Path(
        tempfile.mkdtemp(prefix=f".skillcli-{skill.metadata['id']}-", dir=root)
    )
    staged = transaction / skill.metadata["id"]
    backup = transaction / "backup"
    try:
        for relative, value in content.items():
            windows_key(relative)
            destination = staged.joinpath(*safe_path(relative).parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            try:
                destination.resolve().relative_to(staged.resolve())
            except ValueError as exc:
                raise ValueError(f"destination escapes staging: {relative}") from exc
            destination.write_bytes(value)
        identities: dict[tuple[int, int], str] = {}
        expected_digests = {
            record["path"]: record["sha256"] for record in skill.metadata["files"]
        }
        for relative, expected_digest in expected_digests.items():
            staged_file = staged.joinpath(*safe_path(relative).parts)
            actual_digest = hashlib.sha256(staged_file.read_bytes()).hexdigest()
            if actual_digest != expected_digest:
                raise ValueError(f"staged checksum mismatch: {relative}")
            stat_result = staged_file.stat()
            identity = (stat_result.st_dev, stat_result.st_ino)
            if identity in identities:
                raise ValueError(
                    f"filesystem alias collision: {relative} and {identities[identity]}"
                )
            identities[identity] = relative
        receipt = {
            "qualifiedId": qualified_id,
            "source": {
                "id": skill.source.config.id,
                "repository": skill.source.config.repository,
                "commit": skill.source.resolve(),
            },
            "version": skill.metadata["version"],
            "files": skill.metadata["files"],
        }
        (staged / ".skillcli.json").write_text(
            json.dumps(receipt, indent=2) + "\n",
            encoding="utf-8",
        )
        if host == "copilot-cowork" and skill.metadata["id"] == "skill-zero":
            (staged / "catalogue.json").write_bytes(snapshot)
        if existed:
            target.replace(backup)
        staged.replace(target)
        if backup.exists():
            shutil.rmtree(backup)
        transaction.rmdir()
    except OSError:
        if backup.exists() and not target.exists():
            backup.replace(target)
        if transaction.exists():
            shutil.rmtree(transaction)
        raise
    return str(target), "updated" if existed else "installed"


def install_or_update(catalogues: Catalogues, qualified_id: str, update: bool) -> None:
    if not QUALIFIED_ID_PATTERN.fullmatch(qualified_id):
        raise ValueError("skill ID must use OWNER/REPO/skill-id")
    skill = catalogues.skill(qualified_id)
    metadata = skill.metadata
    if metadata["status"] != "approved" or metadata["review"]["state"] != "approved":
        raise ValueError(f"skill is not approved: {qualified_id}")
    content = remote_content(skill)
    rows = []
    snapshot = catalogues.snapshot()
    for host, root in destinations().items():
        destination, status = replace_folder(
            root,
            qualified_id,
            skill,
            content,
            snapshot,
            host,
            allow_existing=update,
        )
        rows.append(
            [
                host,
                status,
                metadata["version"],
                skill.source.config.id,
                destination,
            ]
        )
    print(table(["Host", "Status", "Version", "Source", "Destination"], rows))
    show_warnings(catalogues)


def remove(qualified_id: str) -> None:
    if not QUALIFIED_ID_PATTERN.fullmatch(qualified_id):
        raise ValueError("skill ID must use OWNER/REPO/skill-id")
    rows = []
    for host, root in destinations().items():
        target = root / qualified_folder(qualified_id)
        if not target.exists():
            rows.append([host, "not installed", str(target)])
            continue
        reject_symlinks(target)
        receipt_path = target / ".skillcli.json"
        try:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            rows.append([host, "refused: missing source metadata", str(target)])
            continue
        if receipt.get("qualifiedId") != qualified_id:
            rows.append([host, "refused: namespace mismatch", str(target)])
            continue
        transaction = Path(
            tempfile.mkdtemp(prefix=".skillcli-remove-", dir=root)
        )
        target.replace(transaction / qualified_folder(qualified_id))
        shutil.rmtree(transaction)
        rows.append([host, "removed", str(target)])
    print(table(["Host", "Status", "Destination"], rows))


def installed_ids() -> set[str]:
    values = set()
    for root in destinations().values():
        if not root.exists():
            continue
        for folder in root.iterdir():
            receipt_path = folder / ".skillcli.json"
            if folder.is_dir() and receipt_path.is_file():
                try:
                    qualified_id = json.loads(
                        receipt_path.read_text(encoding="utf-8")
                    ).get("qualifiedId")
                except (OSError, json.JSONDecodeError):
                    continue
                if isinstance(qualified_id, str):
                    values.add(qualified_id)
    return values


def update(catalogues: Catalogues, skill_id: str | None, update_all: bool) -> None:
    skill_ids = sorted(installed_ids()) if update_all else [skill_id]
    if not skill_ids or skill_ids == [None]:
        raise ValueError("no installed skills were found")
    for index, value in enumerate(skill_ids):
        if index:
            print()
        assert value
        print(f"[{value}]")
        install_or_update(catalogues, value, update=True)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(prog="skillcli")
    commands = result.add_subparsers(dest="command", required=True)
    search_parser = commands.add_parser("search")
    search_parser.add_argument("--role", required=True)
    search_parser.add_argument("--query", required=True)
    install_parser = commands.add_parser("install")
    install_parser.add_argument("--skill", required=True)
    remove_parser = commands.add_parser("remove")
    remove_parser.add_argument("--skill", required=True)
    update_parser = commands.add_parser("update")
    selection = update_parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--skill")
    selection.add_argument("--all", action="store_true")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "search":
            search(Catalogues(), args.role, args.query)
        elif args.command == "install":
            install_or_update(Catalogues(), args.skill, update=False)
        elif args.command == "remove":
            remove(args.skill)
        elif args.command == "update":
            update(Catalogues(), args.skill, args.all)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"skillcli error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
