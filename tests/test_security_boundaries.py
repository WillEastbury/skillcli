from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
os.environ["SKILLCLI_DISABLE_NATIVE_COPILOT"] = "1"
CORE = runpy.run_path(
    str(
        ROOT
        / "plugins"
        / "skillcli-skill-zero"
        / "scripts"
        / "skillcli_core.py"
    )
)
MARKETPLACE = runpy.run_path(str(ROOT / "tools" / "render_marketplace.py"))


class FakeSource:
    def __init__(self, files: dict[str, bytes], repository: str = "Owner/Repo") -> None:
        self.files = files
        self.config = SimpleNamespace(id="test", repository=repository)
        self.read_paths: list[str] = []

    def read(self, path: str) -> bytes:
        self.read_paths.append(path)
        return self.files[path]

    def resolve(self) -> str:
        return "a" * 40


class FakeCatalogues:
    def __init__(self, plugin) -> None:
        self._plugin = plugin

    def plugin(self, qualified_id: str):
        if qualified_id != self._plugin.qualified_id:
            raise ValueError(qualified_id)
        return self._plugin


def plugin(
    records: list[dict[str, str]],
    files: dict[str, bytes],
    repository: str = "Owner/Repo",
):
    manifest = {
        "name": "example-plugin",
        "description": "Example plugin",
        "version": "1.0.0",
        "keywords": ["example"],
    }
    metadata = {
        "skillId": "example-skill",
        "skillRoot": "skills/example-skill",
        "roles": ["developer"],
        "taskCategories": ["testing"],
        "runtime": {"language": "none", "dependencies": []},
        "capabilities": {
            "network": [],
            "filesystem": "none",
            "shell": [],
            "mcpServers": [],
            "authentication": [],
        },
        "review": {"state": "approved"},
        "files": records,
    }
    return CORE["Plugin"](
        FakeSource(files, repository),
        "example-marketplace",
        "plugins/example-plugin",
        manifest,
        metadata,
    )


class WindowsPathTests(unittest.TestCase):
    def test_accepts_normal_nested_path(self) -> None:
        self.assertEqual(
            CORE["windows_key"]("src/helper-script.py"),
            "src/helper-script.py",
        )

    def test_rejects_path_escape_and_absolute_forms(self) -> None:
        for value in (
            "../outside.md",
            "folder/../outside.md",
            "/absolute.md",
            r"folder\outside.md",
        ):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    CORE["windows_key"](value)

    def test_rejects_windows_alias_and_invalid_forms(self) -> None:
        values = (
            "SKILL.md.",
            "SKILL.md ",
            "name:stream",
            "bad<name>.md",
            'bad"name.md',
            "bad|name.md",
            "bad?name.md",
            "bad*name.md",
            "CON",
            "NUL.txt",
            "COM1.py",
            "LPT9.md",
            "SUPPOR~1.PY",
            "control\x01name.md",
        )
        for value in values:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    CORE["windows_key"](value)

    def test_repository_namespaces_produce_distinct_folders(self) -> None:
        public = CORE["qualified_folder"]("Owner/Public/example-plugin")
        private = CORE["qualified_folder"]("Owner/Private/example-plugin")
        self.assertEqual(public, "Owner!Public!example-plugin")
        self.assertEqual(private, "Owner!Private!example-plugin")
        self.assertNotEqual(public, private)

    def test_unqualified_plugin_id_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            CORE["qualified_folder"]("example-plugin")


class ChecksumTests(unittest.TestCase):
    def test_plugin_content_reads_only_declared_skill_files(self) -> None:
        skill_md = b"---\nname: \"Example\"\n---\n"
        helper = b"print('example')\n"
        files = {
            "plugins/example-plugin/skills/example-skill/SKILL.md": skill_md,
            "plugins/example-plugin/skills/example-skill/src/helper.py": helper,
            "plugins/example-plugin/undeclared.txt": b"must not be read",
        }
        records = [
            {
                "path": "skills/example-skill/SKILL.md",
                "sha256": hashlib.sha256(skill_md).hexdigest(),
                "target": "skill",
            },
            {
                "path": "skills/example-skill/src/helper.py",
                "sha256": hashlib.sha256(helper).hexdigest(),
                "target": "skill",
            },
        ]
        selected = plugin(records, files)
        content = CORE["plugin_skill_content"](selected)
        self.assertEqual(set(content), {"SKILL.md", "src/helper.py"})
        self.assertNotIn(
            "plugins/example-plugin/undeclared.txt",
            selected.source.read_paths,
        )

    def test_checksum_mismatch_is_rejected(self) -> None:
        content = b"reviewed content"
        files = {
            "plugins/example-plugin/skills/example-skill/SKILL.md": content
        }
        records = [
            {
                "path": "skills/example-skill/SKILL.md",
                "sha256": "0" * 64,
                "target": "skill",
            }
        ]
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            CORE["plugin_skill_content"](plugin(records, files))

    def test_case_folded_declared_paths_are_rejected(self) -> None:
        content = b"same"
        files = {
            "plugins/example-plugin/skills/example-skill/SKILL.md": content
        }
        digest = hashlib.sha256(content).hexdigest()
        records = [
            {
                "path": "skills/example-skill/SKILL.md",
                "sha256": digest,
                "target": "skill",
            },
            {
                "path": "skills/example-skill/skill.md",
                "sha256": digest,
                "target": "skill",
            },
        ]
        with self.assertRaisesRegex(ValueError, "Windows path collision"):
            CORE["plugin_skill_content"](plugin(records, files))

    def test_text_checksum_normalizes_line_endings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "SKILL.md"
            path.write_bytes(b"line one\r\nline two\r\n")
            windows = hashlib.sha256(MARKETPLACE["canonical_bytes"](path)).hexdigest()
            path.write_bytes(b"line one\nline two\n")
            unix = hashlib.sha256(MARKETPLACE["canonical_bytes"](path)).hexdigest()
            self.assertEqual(windows, unix)


class SourceBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.content = b"---\nskill-id: \"example-skill\"\n---\n"
        self.digest = hashlib.sha256(self.content).hexdigest()
        self.records = [
            {
                "path": "skills/example-skill/SKILL.md",
                "sha256": self.digest,
                "target": "skill",
            }
        ]
        self.plugin = plugin(
            self.records,
            {
                "plugins/example-plugin/skills/example-skill/SKILL.md": self.content
            },
        )

    def test_install_writes_source_bound_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination, status = CORE["replace_folder"](
                root,
                self.plugin,
                {"SKILL.md": self.content},
                b"{}",
                "scout",
                False,
            )
            self.assertEqual(status, "installed")
            receipt = json.loads(
                (Path(destination) / ".skillcli.json").read_text(encoding="utf-8")
            )
            self.assertEqual(receipt["qualifiedId"], self.plugin.qualified_id)
            self.assertEqual(receipt["source"]["repository"], "Owner/Repo")
            self.assertEqual(receipt["files"][0]["relativePath"], "SKILL.md")

    def test_update_rejects_source_namespace_change(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CORE["qualified_folder"](self.plugin.qualified_id)
            target.mkdir()
            (target / "SKILL.md").write_bytes(self.content)
            (target / ".skillcli.json").write_text(
                json.dumps(
                    {
                        "qualifiedId": self.plugin.qualified_id,
                        "source": {"repository": "Attacker/Repo"},
                        "files": [
                            {
                                "relativePath": "SKILL.md",
                                "sha256": self.digest,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            _, status = CORE["replace_folder"](
                root,
                self.plugin,
                {"SKILL.md": self.content},
                b"{}",
                "scout",
                True,
            )
            self.assertEqual(status, "source namespace mismatch")

    def test_update_rejects_modified_managed_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CORE["qualified_folder"](self.plugin.qualified_id)
            target.mkdir()
            (target / "SKILL.md").write_bytes(b"modified")
            (target / ".skillcli.json").write_text(
                json.dumps(
                    {
                        "qualifiedId": self.plugin.qualified_id,
                        "source": {"repository": "Owner/Repo"},
                        "files": [
                            {
                                "relativePath": "SKILL.md",
                                "sha256": self.digest,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            _, status = CORE["replace_folder"](
                root,
                self.plugin,
                {"SKILL.md": self.content},
                b"{}",
                "scout",
                True,
            )
            self.assertEqual(status, "local files modified")

    def test_update_allows_removal_of_previously_managed_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CORE["qualified_folder"](self.plugin.qualified_id)
            target.mkdir()
            old_content = b"old managed file"
            (target / "SKILL.md").write_bytes(self.content)
            (target / "old.txt").write_bytes(old_content)
            (target / ".skillcli.json").write_text(
                json.dumps(
                    {
                        "qualifiedId": self.plugin.qualified_id,
                        "source": {"repository": "Owner/Repo"},
                        "files": [
                            {
                                "relativePath": "SKILL.md",
                                "sha256": self.digest,
                            },
                            {
                                "relativePath": "old.txt",
                                "sha256": hashlib.sha256(old_content).hexdigest(),
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            )
            _, status = CORE["replace_folder"](
                root,
                self.plugin,
                {"SKILL.md": self.content},
                b"{}",
                "scout",
                True,
            )
            self.assertEqual(status, "updated")
            self.assertFalse((target / "old.txt").exists())

    def test_remove_refuses_namespace_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CORE["qualified_folder"](self.plugin.qualified_id)
            target.mkdir()
            (target / ".skillcli.json").write_text(
                json.dumps({"qualifiedId": "Other/Repo/example-plugin"}),
                encoding="utf-8",
            )
            globals_dict = CORE["filesystem_destinations"].__globals__
            original = globals_dict["filesystem_destinations"]
            globals_dict["filesystem_destinations"] = lambda: {"test": root}
            try:
                rows = CORE["remove_plugin"](
                    FakeCatalogues(self.plugin),
                    self.plugin.qualified_id,
                )
                self.assertEqual(rows[0]["status"], "refused: namespace mismatch")
                self.assertTrue(target.exists())
            finally:
                globals_dict["filesystem_destinations"] = original

    def test_reparse_point_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child = root / "child"
            child.mkdir()
            globals_dict = CORE["reject_links"].__globals__
            original = globals_dict["is_reparse_point"]
            globals_dict["is_reparse_point"] = lambda path: path == child
            try:
                with self.assertRaisesRegex(ValueError, "link/reparse point"):
                    CORE["reject_links"](root)
            finally:
                globals_dict["is_reparse_point"] = original


class NativeCopilotAdapterTests(unittest.TestCase):
    def test_native_install_uses_marketplace_interface(self) -> None:
        calls: list[list[str]] = []
        globals_dict = CORE["native_install_or_update"].__globals__
        original_ensure = globals_dict["ensure_native_marketplace"]
        original_names = globals_dict["native_plugin_names"]
        original_run = globals_dict["run_copilot"]
        globals_dict["ensure_native_marketplace"] = lambda selected: None
        globals_dict["native_plugin_names"] = lambda: set()
        globals_dict["run_copilot"] = lambda args: calls.append(args)
        try:
            status = CORE["native_install_or_update"](self._plugin(), False)
            self.assertEqual(status, "installed")
            self.assertEqual(
                calls,
                [["plugin", "install", "example-plugin@example-marketplace"]],
            )
        finally:
            globals_dict["ensure_native_marketplace"] = original_ensure
            globals_dict["native_plugin_names"] = original_names
            globals_dict["run_copilot"] = original_run

    def test_native_update_and_remove_use_plugin_name(self) -> None:
        calls: list[list[str]] = []
        selected = self._plugin()
        globals_dict = CORE["native_install_or_update"].__globals__
        originals = {
            "ensure_native_marketplace": globals_dict["ensure_native_marketplace"],
            "native_plugin_names": globals_dict["native_plugin_names"],
            "run_copilot": globals_dict["run_copilot"],
        }
        globals_dict["ensure_native_marketplace"] = lambda plugin: None
        globals_dict["native_plugin_names"] = lambda: {"example-plugin"}
        globals_dict["run_copilot"] = lambda args: calls.append(args)
        try:
            self.assertEqual(
                CORE["native_install_or_update"](selected, True),
                "updated",
            )
            self.assertEqual(CORE["native_remove"](selected), "removed")
            self.assertEqual(
                calls,
                [
                    ["plugin", "update", "example-plugin"],
                    ["plugin", "uninstall", "example-plugin"],
                ],
            )
        finally:
            globals_dict.update(originals)

    @staticmethod
    def _plugin():
        return plugin(
            [
                {
                    "path": "skills/example-skill/SKILL.md",
                    "sha256": hashlib.sha256(b"x").hexdigest(),
                    "target": "skill",
                }
            ],
            {"plugins/example-plugin/skills/example-skill/SKILL.md": b"x"},
        )


class MacOSDestinationTests(unittest.TestCase):
    def test_detects_macos_scout_and_cowork_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            (home / ".copilot").mkdir()
            one_drive = (
                home
                / "Library"
                / "CloudStorage"
                / "OneDrive-Contoso"
                / "Documents"
                / "Cowork"
            )
            one_drive.mkdir(parents=True)
            globals_dict = CORE["filesystem_destinations"].__globals__
            original_platform = globals_dict["sys"].platform
            globals_dict["sys"].platform = "darwin"
            try:
                with mock.patch.dict(
                    os.environ,
                    {"SKILLCLI_DISABLE_NATIVE_COPILOT": "1"},
                    clear=True,
                ):
                    with mock.patch.object(Path, "home", return_value=home):
                        destinations = CORE["filesystem_destinations"]()
                self.assertEqual(
                    destinations["scout"],
                    home / ".copilot" / "m-skills",
                )
                self.assertEqual(
                    destinations["copilot-cowork"],
                    one_drive / "Skills",
                )
            finally:
                globals_dict["sys"].platform = original_platform


if __name__ == "__main__":
    unittest.main()
