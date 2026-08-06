"""Validate, regenerate, and publish a plugin as a pull request from a client machine.

This repository family cannot rely on GitHub Actions, so every check that would run in
continuous integration runs locally before the pull request is opened. The script
refuses to push when validation fails, which keeps ``merge`` equivalent to ``publish``.

Typical submission::

    python tools/publish_skill.py --plugin my-new-skill --state pending

Typical maintainer approval::

    python tools/publish_skill.py --plugin my-new-skill --state approved \
        --reviewer "A Maintainer"

Authentication is selected per invocation with ``--gh-user``. The shared ``gh`` active
account is never switched.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from render_marketplace import build
import render_readme
import sync_plugin_metadata
import validate_manifest


def run(arguments: list[str], root: Path, env: dict[str, str] | None = None,
        capture: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        arguments,
        cwd=root,
        env=env,
        text=True,
        capture_output=capture,
        check=False,
    )


def git(arguments: list[str], root: Path, env: dict[str, str] | None = None) -> str:
    result = run(["git", *arguments], root, env)
    if result.returncode:
        raise RuntimeError(
            f"git {' '.join(arguments)} failed: {(result.stderr or '').strip()}"
        )
    return (result.stdout or "").strip()


def gh_environment(gh_user: str | None) -> dict[str, str]:
    """Select an account explicitly instead of switching the shared active account."""
    env = os.environ.copy()
    if not gh_user or env.get("GH_TOKEN"):
        return env
    result = subprocess.run(
        ["gh", "auth", "token", "--hostname", "github.com", "--user", gh_user],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(
            (result.stderr or "").strip() or f"cannot obtain a token for {gh_user}"
        )
    env["GH_TOKEN"] = result.stdout.strip()
    return env


def local_checks(root: Path) -> None:
    """Run the checks that continuous integration would otherwise perform."""
    errors = validate_manifest.validate_sources(root)
    for warning in validate_manifest.layout_warnings(root):
        print(f"Warning: {warning}", file=sys.stderr)
    expected = build(root, update=False)
    actual = json.loads((root / "skills.json").read_text(encoding="utf-8"))
    if actual != expected:
        errors.append("skills.json is stale; run python tools/render_marketplace.py")
    if errors:
        raise RuntimeError("; ".join(errors))
    tests = root / "tests"
    if tests.is_dir():
        suite = unittest.defaultTestLoader.discover(str(tests))
        result = unittest.TextTestRunner(verbosity=1).run(suite)
        if not result.wasSuccessful():
            raise RuntimeError("security regression tests failed")


def catalogue_row(root: Path, plugin_name: str) -> str:
    manifest = build(root, update=False)
    namespace = manifest["library"]["namespace"]
    for skill in manifest["skills"]:
        if skill["id"] == plugin_name:
            return render_readme.table([skill], namespace)
    raise RuntimeError(f"{plugin_name} is not present in the generated catalogue")


def regenerate(root: Path) -> None:
    manifest = build(root, update=True)
    readme_path = root / "README.md"
    catalogue = json.loads((root / "catalogue.json").read_text(encoding="utf-8"))
    selected = catalogue["library"].get("readmePlugins")
    skills = (
        manifest["skills"]
        if selected is None
        else [skill for skill in manifest["skills"] if skill["id"] in set(selected)]
    )
    readme_path.write_text(
        render_readme.replace_catalog(
            readme_path.read_text(encoding="utf-8"),
            render_readme.table(skills, manifest["library"]["namespace"]),
        ),
        encoding="utf-8",
    )
    (root / "SKILLS.md").write_text(
        render_readme.full_catalogue(manifest),
        encoding="utf-8",
    )


def pull_request_body(plugin_name: str, state: str, row: str) -> str:
    return "\n".join(
        [
            f"Publishes `{plugin_name}` with review state `{state}`.",
            "",
            "This repository does not run GitHub Actions, so the following checks were",
            "run on the submitting machine before the branch was pushed:",
            "",
            "- `validate_manifest` source, layout, and checksum validation",
            "- generated `skills.json` freshness",
            "- security regression tests",
            "",
            "### Catalogue row",
            "",
            row,
            "",
            "### Maintainer checklist",
            "",
            "- [ ] Skill content contains no secrets, customer data, or personal data.",
            "- [ ] Capabilities, dependencies, MCP servers, and authentication are complete.",
            "- [ ] Review state set to `approved` before merge:",
            f"      `python tools/publish_skill.py --plugin {plugin_name} --state approved --reviewer \"<maintainer>\"`",
            "",
            "Merging this pull request publishes the skill immediately.",
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--state", choices=sync_plugin_metadata.STATES, default="pending")
    parser.add_argument("--version")
    parser.add_argument("--reviewer", action="append", dest="reviewers")
    parser.add_argument("--branch")
    parser.add_argument("--base", default="main")
    parser.add_argument("--gh-user", help="GitHub account to authenticate as")
    parser.add_argument("--repo", help="target OWNER/REPO; defaults to the git remote")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate in a throwaway copy without touching the working tree",
    )
    args = parser.parse_args()
    root = args.root.resolve()

    if args.dry_run:
        with tempfile.TemporaryDirectory() as scratch:
            mirror = Path(scratch) / root.name
            shutil.copytree(
                root,
                mirror,
                ignore=shutil.ignore_patterns(".git", "__pycache__"),
            )
            return preview(mirror, args)

    return publish(root, args)


def preview(root: Path, args: argparse.Namespace) -> int:
    try:
        changed = sync_plugin_metadata.apply(
            root, args.plugin, args.version, args.state, args.reviewers, None
        )
        regenerate(root)
        local_checks(root)
        row = catalogue_row(root, args.plugin)
    except (OSError, ValueError, KeyError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"Publish blocked: {exc}", file=sys.stderr)
        return 1
    for entry in changed:
        print(f"{args.plugin}: would set {entry}")
    print("Local validation passed")
    print("Dry run: working tree untouched, no branch or pull request created")
    print(row)
    return 0


def publish(root: Path, args: argparse.Namespace) -> int:
    try:
        changed = sync_plugin_metadata.apply(
            root,
            args.plugin,
            args.version,
            args.state,
            args.reviewers,
            None,
        )
        regenerate(root)
        local_checks(root)
        row = catalogue_row(root, args.plugin)
    except (OSError, ValueError, KeyError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"Publish blocked: {exc}", file=sys.stderr)
        return 1

    for entry in changed:
        print(f"{args.plugin}: {entry}")
    print("Local validation passed")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d%H%M%S")
    branch = args.branch or f"skill-intake/{args.plugin}-{stamp}"
    env = None
    try:
        env = gh_environment(args.gh_user)
        git(["switch", "-c", branch], root)
        git(["add", "-A"], root)
        git(
            [
                "-c",
                "user.name=skillcli",
                "-c",
                "user.email=skillcli@users.noreply.github.com",
                "commit",
                "-m",
                f"Publish {args.plugin} ({args.state})",
            ],
            root,
        )
        git(["push", "--set-upstream", "origin", branch], root, env)
        command = [
            "gh",
            "pr",
            "create",
            "--base",
            args.base,
            "--head",
            branch,
            "--title",
            f"Publish {args.plugin} ({args.state})",
            "--body",
            pull_request_body(args.plugin, args.state, row),
        ]
        if args.repo:
            command += ["--repo", args.repo]
        result = run(command, root, env)
        if result.returncode:
            raise RuntimeError(
                (result.stderr or "").strip()
                or "pull request creation failed; the branch is pushed and recoverable"
            )
        print((result.stdout or "").strip())
    except (OSError, RuntimeError) as exc:
        print(f"Publish failed: {exc}", file=sys.stderr)
        print(f"Branch retained for recovery: {branch}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
