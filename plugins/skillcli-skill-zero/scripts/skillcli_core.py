"""Shared marketplace and installation logic for skillcli."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
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
    ]
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


def reject_links(path: Path) -> None:
    current = path
    while current != current.parent:
        if current.exists() and (current.is_symlink() or is_reparse_point(current)):
            raise ValueError(f"destination contains a link/reparse point: {current}")
        current = current.parent
    if path.exists():
        for child in path.rglob("*"):
            if child.is_symlink() or is_reparse_point(child):
                raise ValueError(f"destination contains a link/reparse point: {child}")


def run(arguments: list[str], timeout: int = 60) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


def gh(arguments: list[str]) -> bytes:
    result = run(["gh", *arguments])
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
        candidates.append(Path(local_app_data) / "skillcli" / "sources.json")
        candidates.append(
            Path(local_app_data) / "DigitalNativeSkillsLibrary" / "sources.json"
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
        self._cache: dict[str, bytes] = {}

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
        if relative in self._cache:
            return self._cache[relative]
        commit = self.resolve()
        if self.config.private:
            endpoint = (
                f"repos/{self.config.repository}/contents/{quote(relative, safe='/')}"
                f"?ref={quote(commit, safe='')}"
            )
            value = gh(
                [
                    "api",
                    "-H",
                    "Accept: application/vnd.github.raw+json",
                    endpoint,
                ]
            )
        else:
            value = public_bytes(
                f"https://raw.githubusercontent.com/"
                f"{self.config.repository}/{commit}/{quote(relative, safe='/')}"
            )
        self._cache[relative] = value
        return value

    def read_json(self, path: str) -> dict[str, Any]:
        value = json.loads(self.read(path).decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError(f"{path} must contain a JSON object")
        return value


@dataclass(frozen=True)
class Plugin:
    source: Source
    marketplace_name: str
    path: str
    manifest: dict[str, Any]
    metadata: dict[str, Any]

    @property
    def qualified_id(self) -> str:
        return f"{self.source.config.repository}/{self.manifest['name']}"


class Catalogues:
    def __init__(self) -> None:
        config = load_config()
        values = config.get("sources")
        if not isinstance(values, list) or not values:
            raise ValueError("source configuration must contain sources")
        self.sources = [
            Source(
                SourceConfig(
                    id=value["id"],
                    repository=value["repository"],
                    ref=os.environ.get(
                        "SKILLCLI_SOURCE_REF_OVERRIDE",
                        value.get("ref", "main"),
                    ),
                    private=bool(value.get("private", False)),
                )
            )
            for value in values
        ]
        self.warnings: list[str] = []
        self.plugins: dict[str, Plugin] = {}
        self._load()

    def _load(self) -> None:
        available = 0
        for source in self.sources:
            try:
                catalogue = source.read_json("catalogue.json")
                marketplace = source.read_json(".github/plugin/marketplace.json")
            except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
                self.warnings.append(f"{source.config.id}: {exc}")
                continue
            namespace = catalogue.get("library", {}).get("namespace")
            if namespace != source.config.repository:
                self.warnings.append(
                    f"{source.config.id}: namespace {namespace!r} does not match "
                    f"repository {source.config.repository!r}"
                )
                continue
            marketplace_name = marketplace.get("name")
            entries = marketplace.get("plugins")
            if not isinstance(marketplace_name, str) or not isinstance(entries, list):
                self.warnings.append(f"{source.config.id}: invalid marketplace.json")
                continue
            available += 1
            for entry in entries:
                if not isinstance(entry, dict) or not isinstance(entry.get("source"), str):
                    self.warnings.append(
                        f"{source.config.id}: only repository-local plugin sources are supported"
                    )
                    continue
                plugin_path = safe_path(entry["source"]).as_posix()
                try:
                    manifest = source.read_json(f"{plugin_path}/plugin.json")
                    metadata = source.read_json(f"{plugin_path}/skillcli.json")
                except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
                    self.warnings.append(f"{source.config.id}/{plugin_path}: {exc}")
                    continue
                if (
                    manifest.get("name") != entry.get("name")
                    or manifest.get("version") != entry.get("version")
                ):
                    self.warnings.append(
                        f"{source.config.id}/{plugin_path}: marketplace/plugin mismatch"
                    )
                    continue
                plugin = Plugin(
                    source,
                    marketplace_name,
                    plugin_path,
                    manifest,
                    metadata,
                )
                if plugin.qualified_id in self.plugins:
                    raise ValueError(f"duplicate qualified plugin ID: {plugin.qualified_id}")
                self.plugins[plugin.qualified_id] = plugin
        if not available:
            raise RuntimeError("no configured plugin marketplace could be loaded")

    def plugin(self, qualified_id: str) -> Plugin:
        try:
            return self.plugins[qualified_id]
        except KeyError as exc:
            raise ValueError(f"unknown plugin: {qualified_id}") from exc

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
            "plugins": [
                {
                    "qualifiedId": qualified_id,
                    "name": plugin.manifest["name"],
                    "description": plugin.manifest.get("description", ""),
                    "version": plugin.manifest.get("version"),
                    "source": plugin.source.config.id,
                    "metadata": plugin.metadata,
                }
                for qualified_id, plugin in sorted(self.plugins.items())
            ],
        }
        return (json.dumps(value, indent=2) + "\n").encode("utf-8")


def one_drive_root() -> Path | None:
    for variable in ("OneDriveCommercial", "OneDrive", "OneDriveConsumer"):
        value = os.environ.get(variable)
        if value and Path(value).exists():
            return Path(value)
    return None


def native_copilot_available() -> bool:
    return (
        not os.environ.get("SKILLCLI_DESTINATIONS")
        and os.environ.get("SKILLCLI_DISABLE_NATIVE_COPILOT") != "1"
        and shutil.which("copilot") is not None
    )


def filesystem_destinations() -> dict[str, Path]:
    if os.environ.get("SKILLCLI_DISABLE_FILESYSTEM") == "1":
        return {}
    override = os.environ.get("SKILLCLI_DESTINATIONS")
    if override:
        value = json.loads(override)
        if not isinstance(value, dict):
            raise ValueError("SKILLCLI_DESTINATIONS must be a JSON object")
        return {name: Path(path).expanduser().resolve() for name, path in value.items()}
    home = Path.home()
    result: dict[str, Path] = {}
    if not native_copilot_available() and (home / ".copilot").exists():
        result["copilot-cli-filesystem"] = home / ".copilot" / "skills"
    if (home / ".scout").exists():
        result["scout"] = home / ".scout" / "m-skills"
    drive = one_drive_root()
    if drive and (drive / "Documents" / "Cowork").exists():
        result["copilot-cowork"] = drive / "Documents" / "Cowork" / "Skills"
    return result


def run_copilot(arguments: list[str]) -> subprocess.CompletedProcess[bytes]:
    result = run(["copilot", *arguments], timeout=120)
    if result.returncode:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(message or f"copilot exited with code {result.returncode}")
    return result


def json_contains_name(value: Any, name: str) -> bool:
    if isinstance(value, dict):
        if value.get("name") == name:
            return True
        return any(json_contains_name(item, name) for item in value.values())
    if isinstance(value, list):
        return any(json_contains_name(item, name) for item in value)
    return False


def ensure_native_marketplace(plugin: Plugin) -> None:
    result = run(["copilot", "plugin", "marketplace", "list", "--json"], timeout=120)
    found = False
    if result.returncode == 0:
        try:
            found = json_contains_name(
                json.loads(result.stdout.decode("utf-8")),
                plugin.marketplace_name,
            )
        except json.JSONDecodeError:
            found = False
    if not found:
        plain = run_copilot(["plugin", "marketplace", "list"])
        found = plugin.marketplace_name.casefold() in plain.stdout.decode(
            "utf-8",
            errors="replace",
        ).casefold()
    if not found:
        specification = plugin.source.config.repository
        if plugin.source.config.ref != "main":
            specification += f"#{plugin.source.config.ref}"
        run_copilot(
            [
                "plugin",
                "marketplace",
                "add",
                specification,
            ]
        )


def native_plugin_names() -> set[str]:
    if not native_copilot_available():
        return set()
    result = run_copilot(["plugin", "list"])
    text = result.stdout.decode("utf-8", errors="replace")
    names: set[str] = set()
    for line in text.splitlines():
        for token in re.findall(r"[A-Za-z0-9][A-Za-z0-9.-]*", line):
            names.add(token)
    return names


def native_install_or_update(plugin: Plugin, update: bool) -> str:
    ensure_native_marketplace(plugin)
    installed = plugin.manifest["name"] in native_plugin_names()
    if update and installed:
        run_copilot(["plugin", "update", plugin.manifest["name"]])
        return "updated"
    if installed:
        return "already installed"
    run_copilot(
        [
            "plugin",
            "install",
            f"{plugin.manifest['name']}@{plugin.marketplace_name}",
        ]
    )
    return "installed"


def native_remove(plugin: Plugin) -> str:
    if plugin.manifest["name"] not in native_plugin_names():
        return "not installed"
    run_copilot(["plugin", "uninstall", plugin.manifest["name"]])
    return "removed"


def requirements(plugin: Plugin) -> str:
    values = []
    capabilities = plugin.metadata["capabilities"]
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
    if plugin.metadata["runtime"]["dependencies"]:
        values.append("dependencies")
    return ", ".join(values) or "none"


def terms(value: str) -> set[str]:
    return {item for item in re.findall(r"[a-z0-9]+", value.lower()) if len(item) > 1}


def search_plugins(catalogues: Catalogues, role: str, query: str) -> list[dict[str, Any]]:
    query_terms = terms(query)
    matches = []
    for qualified_id, plugin in catalogues.plugins.items():
        if plugin.metadata["review"]["state"] != "approved":
            continue
        searchable = terms(
            " ".join(
                [
                    plugin.manifest["name"],
                    plugin.manifest.get("description", ""),
                    *plugin.manifest.get("keywords", []),
                    *plugin.metadata["taskCategories"],
                ]
            )
        )
        matched = query_terms & searchable
        if query_terms and not matched:
            continue
        score = len(matched) * 4 + (3 if role in plugin.metadata["roles"] else 0)
        matches.append(
            {
                "score": score,
                "qualifiedId": qualified_id,
                "name": plugin.manifest["name"],
                "description": plugin.manifest.get("description", ""),
                "version": plugin.manifest.get("version"),
                "source": plugin.source.config.id,
                "why": ", ".join(sorted(matched)) or "role match",
                "requirements": requirements(plugin),
            }
        )
    matches.sort(key=lambda item: (-item["score"], item["qualifiedId"]))
    return matches[:10]


def plugin_skill_content(plugin: Plugin) -> dict[str, bytes]:
    root = safe_path(plugin.metadata["skillRoot"]).as_posix()
    prefix = root + "/"
    content: dict[str, bytes] = {}
    keys: set[str] = set()
    for record in plugin.metadata["files"]:
        if record.get("target") != "skill":
            continue
        path = record.get("path")
        digest = record.get("sha256")
        if not isinstance(path, str) or not isinstance(digest, str):
            raise ValueError(f"invalid file record in {plugin.qualified_id}")
        if not path.startswith(prefix):
            raise ValueError(f"skill file is outside skillRoot: {path}")
        relative = path[len(prefix) :]
        key = windows_key(relative)
        if key in keys:
            raise ValueError(f"Windows path collision: {relative}")
        keys.add(key)
        value = plugin.source.read(f"{plugin.path}/{path}")
        if hashlib.sha256(value).hexdigest() != digest:
            raise ValueError(f"checksum mismatch: {plugin.qualified_id}/{path}")
        content[relative] = value
    if "SKILL.md" not in content:
        raise ValueError(f"plugin does not declare SKILL.md: {plugin.qualified_id}")
    return content


def qualified_folder(qualified_id: str) -> str:
    if not QUALIFIED_ID_PATTERN.fullmatch(qualified_id):
        raise ValueError("plugin ID must use OWNER/REPO/plugin-name")
    return qualified_id.replace("/", "!")


def replace_folder(
    root: Path,
    plugin: Plugin,
    content: dict[str, bytes],
    snapshot: bytes,
    host: str,
    allow_existing: bool,
) -> tuple[str, str]:
    target = root / qualified_folder(plugin.qualified_id)
    reject_links(target)
    existed = target.exists()
    if existed and not allow_existing:
        return str(target), "already installed"
    if existed:
        receipt_path = target / ".skillcli.json"
        if not receipt_path.is_file():
            return str(target), "missing source metadata"
        try:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return str(target), "invalid source metadata"
        if (
            receipt.get("qualifiedId") != plugin.qualified_id
            or receipt.get("source", {}).get("repository")
            != plugin.source.config.repository
        ):
            return str(target), "source namespace mismatch"
        managed_paths = set()
        for record in receipt.get("files", []):
            relative = record.get("relativePath")
            digest = record.get("sha256")
            if not isinstance(relative, str) or not isinstance(digest, str):
                return str(target), "invalid source metadata"
            managed_paths.add(relative)
            local_file = target.joinpath(*safe_path(relative).parts)
            if (
                not local_file.is_file()
                or hashlib.sha256(local_file.read_bytes()).hexdigest() != digest
            ):
                return str(target), "local files modified"
        existing_files = {
            path.relative_to(target).as_posix()
            for path in target.rglob("*")
            if path.is_file()
        }
        unexpected = sorted(
            existing_files
            - set(content)
            - managed_paths
            - {"catalogue.json", ".skillcli.json"}
        )
        if unexpected:
            return str(target), "local files present"

    root.mkdir(parents=True, exist_ok=True)
    transaction = Path(
        tempfile.mkdtemp(prefix=f".skillcli-{plugin.manifest['name']}-", dir=root)
    )
    staged = transaction / qualified_folder(plugin.qualified_id)
    backup = transaction / "backup"
    try:
        for relative, value in content.items():
            destination = staged.joinpath(*safe_path(relative).parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            try:
                destination.resolve().relative_to(staged.resolve())
            except ValueError as exc:
                raise ValueError(f"destination escapes staging: {relative}") from exc
            destination.write_bytes(value)
        identities: dict[tuple[int, int], str] = {}
        receipt_files = []
        for relative, expected_content in content.items():
            staged_file = staged.joinpath(*safe_path(relative).parts)
            digest = hashlib.sha256(staged_file.read_bytes()).hexdigest()
            if digest != hashlib.sha256(expected_content).hexdigest():
                raise ValueError(f"staged checksum mismatch: {relative}")
            identity = (staged_file.stat().st_dev, staged_file.stat().st_ino)
            if identity in identities:
                raise ValueError(
                    f"filesystem alias collision: {relative} and {identities[identity]}"
                )
            identities[identity] = relative
            receipt_files.append({"relativePath": relative, "sha256": digest})
        receipt = {
            "qualifiedId": plugin.qualified_id,
            "pluginName": plugin.manifest["name"],
            "marketplace": plugin.marketplace_name,
            "source": {
                "id": plugin.source.config.id,
                "repository": plugin.source.config.repository,
                "commit": plugin.source.resolve(),
            },
            "version": plugin.manifest["version"],
            "files": receipt_files,
        }
        (staged / ".skillcli.json").write_text(
            json.dumps(receipt, indent=2) + "\n",
            encoding="utf-8",
        )
        if host == "copilot-cowork" and plugin.manifest["name"] == "skillcli-skill-zero":
            (staged / "catalogue.json").write_bytes(snapshot)
        if existed:
            target.replace(backup)
        staged.replace(target)
        if backup.exists():
            shutil.rmtree(backup)
        transaction.rmdir()
    except (OSError, ValueError):
        if backup.exists() and not target.exists():
            backup.replace(target)
        if transaction.exists():
            shutil.rmtree(transaction)
        raise
    return str(target), "updated" if existed else "installed"


def install_or_update(
    catalogues: Catalogues,
    qualified_id: str,
    update: bool,
) -> list[dict[str, str]]:
    plugin = catalogues.plugin(qualified_id)
    if plugin.metadata["review"]["state"] != "approved":
        raise ValueError(f"plugin is not approved: {qualified_id}")
    rows: list[dict[str, str]] = []
    if native_copilot_available():
        rows.append(
            {
                "host": "copilot-cli-native",
                "status": native_install_or_update(plugin, update),
                "version": plugin.manifest["version"],
                "source": plugin.source.config.id,
                "destination": f"{plugin.manifest['name']}@{plugin.marketplace_name}",
            }
        )
    content = plugin_skill_content(plugin)
    snapshot = catalogues.snapshot()
    for host, root in filesystem_destinations().items():
        destination, status = replace_folder(
            root,
            plugin,
            content,
            snapshot,
            host,
            update,
        )
        rows.append(
            {
                "host": host,
                "status": status,
                "version": plugin.manifest["version"],
                "source": plugin.source.config.id,
                "destination": destination,
            }
        )
    if not rows:
        raise ValueError("no supported Copilot CLI, Scout, or Co-Work host was detected")
    return rows


def remove_plugin(catalogues: Catalogues, qualified_id: str) -> list[dict[str, str]]:
    plugin = catalogues.plugin(qualified_id)
    rows: list[dict[str, str]] = []
    if native_copilot_available():
        rows.append(
            {
                "host": "copilot-cli-native",
                "status": native_remove(plugin),
                "destination": plugin.manifest["name"],
            }
        )
    for host, root in filesystem_destinations().items():
        target = root / qualified_folder(qualified_id)
        if not target.exists():
            rows.append({"host": host, "status": "not installed", "destination": str(target)})
            continue
        reject_links(target)
        receipt_path = target / ".skillcli.json"
        try:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            rows.append(
                {
                    "host": host,
                    "status": "refused: missing source metadata",
                    "destination": str(target),
                }
            )
            continue
        if receipt.get("qualifiedId") != qualified_id:
            rows.append(
                {
                    "host": host,
                    "status": "refused: namespace mismatch",
                    "destination": str(target),
                }
            )
            continue
        transaction = Path(
            tempfile.mkdtemp(prefix=".skillcli-remove-", dir=root)
        )
        target.replace(transaction / qualified_folder(qualified_id))
        shutil.rmtree(transaction)
        rows.append({"host": host, "status": "removed", "destination": str(target)})
    return rows


def installed_qualified_ids(catalogues: Catalogues) -> set[str]:
    values: set[str] = set()
    for root in filesystem_destinations().values():
        if not root.exists():
            continue
        for folder in root.iterdir():
            receipt_path = folder / ".skillcli.json"
            if folder.is_dir() and receipt_path.is_file():
                try:
                    value = json.loads(receipt_path.read_text(encoding="utf-8")).get(
                        "qualifiedId"
                    )
                except (OSError, json.JSONDecodeError):
                    continue
                if isinstance(value, str):
                    values.add(value)
    native_names = native_plugin_names()
    for qualified_id, plugin in catalogues.plugins.items():
        if plugin.manifest["name"] in native_names:
            values.add(qualified_id)
    return values
