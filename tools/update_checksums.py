"""Update declared skill file checksums in skills.json."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path, PurePosixPath


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


def windows_key(value: str) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or "\\" in value
        or path.is_absolute()
        or path.as_posix() != value
        or any(part in {"", ".", ".."} for part in path.parts)
    ):
        raise ValueError(f"unsafe relative path: {value!r}")
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


def canonical_bytes(path: Path) -> bytes:
    content = path.read_bytes()
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        return content
    return text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    manifest_path = root / "skills.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for skill in manifest["skills"]:
        folder = root.joinpath(*PurePosixPath(skill["path"]).parts)
        records = []
        seen: set[str] = set()
        for path in sorted(folder.rglob("*")):
            if path.is_dir():
                continue
            if path.is_symlink() or is_reparse_point(path):
                raise ValueError(f"skill contains a link or reparse point: {path}")
            relative = path.relative_to(folder).as_posix()
            key = windows_key(relative)
            if key in seen:
                raise ValueError(f"Windows-canonical path collision: {relative}")
            seen.add(key)
            records.append(
                {
                    "path": relative,
                    "sha256": hashlib.sha256(canonical_bytes(path)).hexdigest(),
                }
            )
        if not records:
            raise ValueError(f"skill folder is empty: {skill['path']}")
        skill["files"] = records
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )
    print(f"Updated checksums for {len(manifest['skills'])} skills")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Checksum update failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
