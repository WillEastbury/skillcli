"""Managed multi-source skill catalogue CLI."""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import quote


ID_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
DEFAULT_CONFIG = {
    "duplicatePolicy": "highest-priority",
    "sources": [
        {
            "id": "public",
            "repository": "WillEastbury/skillcli",
            "ref": "main",
            "priority": 10,
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
    priority: int
    private: bool


class Source:
    def __init__(self, config: SourceConfig) -> None:
        self.config = config
        self.commit: str | None = None
        self.tree_sha: str | None = None
        self._manifest_bytes: bytes | None = None
        self._manifest: dict[str, Any] | None = None
        self._tree: list[dict[str, Any]] | None = None

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
        self.tree_sha = response["commit"]["tree"]["sha"]
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

    def tree(self) -> list[dict[str, Any]]:
        if self._tree is not None:
            return self._tree
        self.resolve()
        assert self.tree_sha
        response = self.api_json(
            f"repos/{self.config.repository}/git/trees/{self.tree_sha}?recursive=1"
        )
        if response.get("truncated"):
            raise RuntimeError(f"{self.config.id} returned a truncated repository tree")
        tree = response.get("tree")
        if not isinstance(tree, list):
            raise RuntimeError(f"{self.config.id} did not return a repository tree")
        self._tree = tree
        return tree

    def files(self, skill: dict[str, Any]) -> list[str]:
        prefix = safe_path(skill["path"]).as_posix() + "/"
        files = []
        for entry in self.tree():
            path = entry.get("path")
            if not isinstance(path, str) or not path.startswith(prefix):
                continue
            if entry.get("type") == "blob" and entry.get("mode") != "120000":
                files.append(path)
            elif entry.get("type") != "tree":
                raise ValueError(
                    f"unsupported Git object in {self.config.id} skill folder: {path}"
                )
        if prefix + skill["entrypoint"] not in files:
            raise ValueError(f"skill entrypoint is missing: {skill['id']}")
        return sorted(files)


@dataclass(frozen=True)
class Skill:
    source: Source
    metadata: dict[str, Any]


class Catalogues:
    def __init__(self) -> None:
        config = load_config()
        if config.get("duplicatePolicy") != "highest-priority":
            raise ValueError("duplicatePolicy must be highest-priority")
        source_values = config.get("sources")
        if not isinstance(source_values, list) or not source_values:
            raise ValueError("source configuration must contain sources")
        self.sources = [
            Source(
                SourceConfig(
                    id=value["id"],
                    repository=value["repository"],
                    ref=value.get("ref", "main"),
                    priority=int(value.get("priority", 0)),
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
        for source in sorted(
            self.sources,
            key=lambda item: item.config.priority,
        ):
            try:
                manifest = source.manifest()
            except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
                self.warnings.append(f"{source.config.id}: {exc}")
                continue
            available += 1
            for metadata in manifest["skills"]:
                skill_id = metadata.get("id")
                if not isinstance(skill_id, str):
                    continue
                current = self.skills.get(skill_id)
                if (
                    current is None
                    or source.config.priority >= current.source.config.priority
                ):
                    self.skills[skill_id] = Skill(source, metadata)
        if not available:
            raise RuntimeError("no configured skill catalogue could be loaded")

    def skill(self, skill_id: str) -> Skill:
        try:
            return self.skills[skill_id]
        except KeyError as exc:
            raise ValueError(f"unknown skill: {skill_id}") from exc

    def snapshot(self) -> bytes:
        value = {
            "sources": [
                {
                    "id": source.config.id,
                    "repository": source.config.repository,
                    "ref": source.config.ref,
                    "priority": source.config.priority,
                }
                for source in self.sources
            ],
            "skills": [
                {
                    **skill.metadata,
                    "catalogueSource": skill.source.config.id,
                }
                for skill in sorted(self.skills.values(), key=lambda item: item.metadata["id"])
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
    for skill in catalogues.skills.values():
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
        matches.append((score, skill, reason))
    matches.sort(key=lambda item: (-item[0], item[1].metadata["id"]))
    rows = [
        [
            rank,
            skill.metadata["id"],
            skill.metadata["name"],
            skill.metadata["version"],
            skill.source.config.id,
            reason,
            requirements(skill.metadata),
        ]
        for rank, (_, skill, reason) in enumerate(matches[:10], start=1)
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
        if current.exists() and current.is_symlink():
            raise ValueError(f"destination contains a symbolic link: {current}")
        current = current.parent
    if path.exists():
        for child in path.rglob("*"):
            if child.is_symlink():
                raise ValueError(f"destination contains a symbolic link: {child}")


def remote_content(skill: Skill) -> dict[str, bytes]:
    prefix = safe_path(skill.metadata["path"]).as_posix() + "/"
    return {
        path[len(prefix) :]: skill.source.read(path)
        for path in skill.source.files(skill.metadata)
    }


def replace_folder(
    root: Path,
    skill: Skill,
    content: dict[str, bytes],
    snapshot: bytes,
    host: str,
    allow_existing: bool,
) -> tuple[str, str]:
    target = root / skill.metadata["id"]
    reject_symlinks(target)
    existed = target.exists()
    if existed and not allow_existing:
        return str(target), "already installed"
    if existed:
        existing = {
            path.relative_to(target).as_posix()
            for path in target.rglob("*")
            if path.is_file()
        }
        unexpected = sorted(existing - set(content) - {"catalogue.json"})
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
            destination = staged.joinpath(*safe_path(relative).parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(value)
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


def install_or_update(catalogues: Catalogues, skill_id: str, update: bool) -> None:
    if not ID_PATTERN.fullmatch(skill_id):
        raise ValueError("skill ID must be lowercase kebab-case")
    skill = catalogues.skill(skill_id)
    metadata = skill.metadata
    if metadata["status"] != "approved" or metadata["review"]["state"] != "approved":
        raise ValueError(f"skill is not approved: {skill_id}")
    content = remote_content(skill)
    rows = []
    snapshot = catalogues.snapshot()
    for host, root in destinations().items():
        destination, status = replace_folder(
            root,
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


def remove(skill_id: str) -> None:
    if not ID_PATTERN.fullmatch(skill_id):
        raise ValueError("skill ID must be lowercase kebab-case")
    rows = []
    for host, root in destinations().items():
        target = root / skill_id
        if not target.exists():
            rows.append([host, "not installed", str(target)])
            continue
        reject_symlinks(target)
        metadata = frontmatter(target / "SKILL.md") if (target / "SKILL.md").is_file() else {}
        if metadata.get("skill-id") != skill_id:
            rows.append([host, "refused: identity mismatch", str(target)])
            continue
        transaction = Path(
            tempfile.mkdtemp(prefix=f".skillcli-remove-{skill_id}-", dir=root)
        )
        target.replace(transaction / skill_id)
        shutil.rmtree(transaction)
        rows.append([host, "removed", str(target)])
    print(table(["Host", "Status", "Destination"], rows))


def installed_ids() -> set[str]:
    values = set()
    for root in destinations().values():
        if not root.exists():
            continue
        for folder in root.iterdir():
            skill_file = folder / "SKILL.md"
            if folder.is_dir() and skill_file.is_file():
                skill_id = frontmatter(skill_file).get("skill-id")
                if skill_id:
                    values.add(skill_id)
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
