from __future__ import annotations

import contextlib
import hashlib
import io
import json
import runpy
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
CLI = runpy.run_path(str(ROOT / "skills" / "skill-zero" / "skillcli.py"))
CHECKSUMS = runpy.run_path(str(ROOT / "tools" / "update_checksums.py"))


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


def skill(
    records: list[dict[str, str]],
    files: dict[str, bytes],
    repository: str = "Owner/Repo",
):
    metadata = {
        "id": "example-skill",
        "version": "1.0.0",
        "path": "skills/example-skill",
        "entrypoint": "SKILL.md",
        "files": records,
    }
    return CLI["Skill"](FakeSource(files, repository), metadata)


class WindowsPathTests(unittest.TestCase):
    def test_accepts_normal_nested_path(self) -> None:
        self.assertEqual(
            CLI["windows_key"]("src/helper-script.py"),
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
                    CLI["windows_key"](value)

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
                    CLI["windows_key"](value)

    def test_repository_namespaces_produce_distinct_folders(self) -> None:
        public = CLI["qualified_folder"]("Owner/Public/same-skill")
        private = CLI["qualified_folder"]("Owner/Private/same-skill")
        self.assertNotEqual(public, private)
        self.assertEqual(public, "Owner!Public!same-skill")
        self.assertEqual(private, "Owner!Private!same-skill")

    def test_unqualified_skill_id_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            CLI["qualified_folder"]("same-skill")


class ChecksumTests(unittest.TestCase):
    def test_remote_content_reads_only_declared_files(self) -> None:
        skill_md = b"---\nname: \"Example\"\n---\n"
        helper = b"print('example')\n"
        files = {
            "skills/example-skill/SKILL.md": skill_md,
            "skills/example-skill/src/helper.py": helper,
            "skills/example-skill/undeclared.txt": b"must not be read",
        }
        records = [
            {
                "path": "SKILL.md",
                "sha256": hashlib.sha256(skill_md).hexdigest(),
            },
            {
                "path": "src/helper.py",
                "sha256": hashlib.sha256(helper).hexdigest(),
            },
        ]
        selected = skill(records, files)
        content = CLI["remote_content"](selected)
        self.assertEqual(set(content), {"SKILL.md", "src/helper.py"})
        self.assertNotIn(
            "skills/example-skill/undeclared.txt",
            selected.source.read_paths,
        )

    def test_checksum_mismatch_is_rejected(self) -> None:
        content = b"reviewed content"
        files = {"skills/example-skill/SKILL.md": content}
        records = [{"path": "SKILL.md", "sha256": "0" * 64}]
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            CLI["remote_content"](skill(records, files))

    def test_case_folded_declared_paths_are_rejected(self) -> None:
        content = b"same"
        files = {"skills/example-skill/SKILL.md": content}
        digest = hashlib.sha256(content).hexdigest()
        records = [
            {"path": "SKILL.md", "sha256": digest},
            {"path": "skill.md", "sha256": digest},
        ]
        with self.assertRaisesRegex(ValueError, "Windows path collision"):
            CLI["remote_content"](skill(records, files))

    def test_text_checksum_normalizes_line_endings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "SKILL.md"
            path.write_bytes(b"line one\r\nline two\r\n")
            windows = hashlib.sha256(CHECKSUMS["canonical_bytes"](path)).hexdigest()
            path.write_bytes(b"line one\nline two\n")
            unix = hashlib.sha256(CHECKSUMS["canonical_bytes"](path)).hexdigest()
            self.assertEqual(windows, unix)


class SourceBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.content = b"---\nskill-id: \"example-skill\"\n---\n"
        self.digest = hashlib.sha256(self.content).hexdigest()
        self.records = [{"path": "SKILL.md", "sha256": self.digest}]
        self.skill = skill(
            self.records,
            {"skills/example-skill/SKILL.md": self.content},
        )
        self.qualified_id = "Owner/Repo/example-skill"

    def test_install_writes_source_bound_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination, status = CLI["replace_folder"](
                root,
                self.qualified_id,
                self.skill,
                {"SKILL.md": self.content},
                b"{}",
                "copilot-cli",
                False,
            )
            self.assertEqual(status, "installed")
            receipt = json.loads(
                (Path(destination) / ".skillcli.json").read_text(encoding="utf-8")
            )
            self.assertEqual(receipt["qualifiedId"], self.qualified_id)
            self.assertEqual(receipt["source"]["repository"], "Owner/Repo")
            self.assertEqual(receipt["files"], self.records)

    def test_update_rejects_source_namespace_change(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CLI["qualified_folder"](self.qualified_id)
            target.mkdir()
            (target / "SKILL.md").write_bytes(self.content)
            (target / ".skillcli.json").write_text(
                json.dumps(
                    {
                        "qualifiedId": self.qualified_id,
                        "source": {"repository": "Attacker/Repo"},
                        "files": self.records,
                    }
                ),
                encoding="utf-8",
            )
            _, status = CLI["replace_folder"](
                root,
                self.qualified_id,
                self.skill,
                {"SKILL.md": self.content},
                b"{}",
                "copilot-cli",
                True,
            )
            self.assertEqual(status, "source namespace mismatch")
            self.assertEqual((target / "SKILL.md").read_bytes(), self.content)

    def test_update_rejects_modified_managed_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CLI["qualified_folder"](self.qualified_id)
            target.mkdir()
            (target / "SKILL.md").write_bytes(b"locally modified")
            (target / ".skillcli.json").write_text(
                json.dumps(
                    {
                        "qualifiedId": self.qualified_id,
                        "source": {"repository": "Owner/Repo"},
                        "files": self.records,
                    }
                ),
                encoding="utf-8",
            )
            _, status = CLI["replace_folder"](
                root,
                self.qualified_id,
                self.skill,
                {"SKILL.md": self.content},
                b"{}",
                "copilot-cli",
                True,
            )
            self.assertEqual(status, "local files modified")
            self.assertEqual((target / "SKILL.md").read_bytes(), b"locally modified")

    def test_update_rejects_existing_namespaced_folder_without_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CLI["qualified_folder"](self.qualified_id)
            target.mkdir()
            (target / "SKILL.md").write_bytes(self.content)
            _, status = CLI["replace_folder"](
                root,
                self.qualified_id,
                self.skill,
                {"SKILL.md": self.content},
                b"{}",
                "copilot-cli",
                True,
            )
            self.assertEqual(status, "missing source metadata")

    def test_remove_refuses_namespace_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / CLI["qualified_folder"](self.qualified_id)
            target.mkdir()
            (target / "SKILL.md").write_bytes(self.content)
            (target / ".skillcli.json").write_text(
                json.dumps({"qualifiedId": "Other/Repo/example-skill"}),
                encoding="utf-8",
            )
            globals_dict = CLI["remove"].__globals__
            original_destinations = globals_dict["destinations"]
            globals_dict["destinations"] = lambda: {"test": root}
            try:
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    CLI["remove"](self.qualified_id)
                self.assertIn("refused: namespace mismatch", output.getvalue())
                self.assertTrue(target.exists())
            finally:
                globals_dict["destinations"] = original_destinations

    def test_reparse_point_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child = root / "child"
            child.mkdir()
            globals_dict = CLI["reject_symlinks"].__globals__
            original = globals_dict["is_reparse_point"]
            globals_dict["is_reparse_point"] = lambda path: path == child
            try:
                with self.assertRaisesRegex(ValueError, "link/reparse point"):
                    CLI["reject_symlinks"](root)
            finally:
                globals_dict["is_reparse_point"] = original


if __name__ == "__main__":
    unittest.main()
