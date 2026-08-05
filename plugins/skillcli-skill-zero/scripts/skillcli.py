"""Command-line interface for governed plugin marketplaces."""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

from skillcli_core import (
    Catalogues,
    install_or_update,
    installed_qualified_ids,
    remove_plugin,
    search_plugins,
)


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


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(prog="skillcli")
    commands = result.add_subparsers(dest="command", required=True)
    search = commands.add_parser("search")
    search.add_argument("--role", required=True)
    search.add_argument("--query", required=True)
    install = commands.add_parser("install")
    install.add_argument("--skill", required=True)
    remove = commands.add_parser("remove")
    remove.add_argument("--skill", required=True)
    update = commands.add_parser("update")
    selection = update.add_mutually_exclusive_group(required=True)
    selection.add_argument("--skill")
    selection.add_argument("--all", action="store_true")
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        catalogues = Catalogues()
        if args.command == "search":
            results = search_plugins(catalogues, args.role, args.query)
            print(
                table(
                    [
                        "#",
                        "Plugin ID",
                        "Name",
                        "Version",
                        "Source",
                        "Why",
                        "Requirements",
                    ],
                    [
                        [
                            index,
                            item["qualifiedId"],
                            item["name"],
                            item["version"],
                            item["source"],
                            item["why"],
                            item["requirements"],
                        ]
                        for index, item in enumerate(results, start=1)
                    ],
                )
            )
        elif args.command == "install":
            rows = install_or_update(catalogues, args.skill, False)
            print(
                table(
                    ["Host", "Status", "Version", "Source", "Destination"],
                    [
                        [
                            row["host"],
                            row["status"],
                            row["version"],
                            row["source"],
                            row["destination"],
                        ]
                        for row in rows
                    ],
                )
            )
        elif args.command == "remove":
            rows = remove_plugin(catalogues, args.skill)
            print(
                table(
                    ["Host", "Status", "Destination"],
                    [
                        [row["host"], row["status"], row["destination"]]
                        for row in rows
                    ],
                )
            )
        elif args.command == "update":
            qualified_ids = (
                sorted(installed_qualified_ids(catalogues))
                if args.all
                else [args.skill]
            )
            if not qualified_ids or qualified_ids == [None]:
                raise ValueError("no installed plugins were found")
            for index, qualified_id in enumerate(qualified_ids):
                if index:
                    print()
                assert qualified_id
                print(f"[{qualified_id}]")
                rows = install_or_update(catalogues, qualified_id, True)
                print(
                    table(
                        ["Host", "Status", "Version", "Source", "Destination"],
                        [
                            [
                                row["host"],
                                row["status"],
                                row["version"],
                                row["source"],
                                row["destination"],
                            ]
                            for row in rows
                        ],
                    )
                )
        show_warnings(catalogues)
        return 0
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"skillcli error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
