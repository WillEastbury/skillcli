"""Normalize an external skill inventory into reviewable catalogue candidates."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


ID_PATTERN = re.compile(r"[^a-z0-9]+")


def nested_value(record: Any, path: str | None, default: Any = None) -> Any:
    if not path:
        return default
    value = record
    for part in path.split("."):
        if isinstance(value, dict) and part in value:
            value = value[part]
        else:
            return default
    return value


def records_from(value: Any, records_path: str | None) -> list[Any]:
    records = nested_value(value, records_path) if records_path else value
    if not isinstance(records, list):
        raise ValueError("configured records collection must be an array")
    return records


def slug(value: str) -> str:
    normalized = ID_PATTERN.sub("-", value.lower()).strip("-")
    return normalized or "unnamed-skill"


def string_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [item.strip() for item in value.split(",") if item.strip()]
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    return [str(value)]


def normalized_score(value: Any, scale: dict[str, Any] | None) -> float | None:
    if scale is None or not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    minimum = scale.get("min")
    maximum = scale.get("max")
    if not isinstance(minimum, (int, float)) or not isinstance(maximum, (int, float)):
        raise ValueError("scoreScale min and max must be numeric")
    if maximum <= minimum:
        raise ValueError("scoreScale max must be greater than min")
    bounded = min(maximum, max(minimum, value))
    return round((bounded - minimum) / (maximum - minimum) * 100, 2)


def fingerprint(name: str, description: str) -> str:
    canonical = " ".join((name + " " + description).lower().split())
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def normalize_record(
    record: Any,
    index: int,
    source_id: str,
    mapping: dict[str, Any],
) -> dict[str, Any]:
    if not isinstance(record, dict):
        raise ValueError(f"record {index} is not an object")
    fields = mapping["fields"]
    original_value = nested_value(record, fields.get("id"))
    original_id = (
        str(original_value).strip()
        if original_value is not None and str(original_value).strip()
        else f"record-{index}"
    )
    name_value = nested_value(record, fields.get("name"))
    name = (
        str(name_value).strip()
        if name_value is not None and str(name_value).strip()
        else original_id
    )
    description_value = nested_value(record, fields.get("description"))
    description = (
        str(description_value).strip()
        if description_value is not None and str(description_value).strip()
        else ""
    )
    raw_taxonomy = string_list(nested_value(record, fields.get("taxonomy")))
    taxonomy_map = mapping.get("taxonomyMap", {})
    normalized_tags = sorted(
        {
            slug(taxonomy_map.get(value, value))
            for value in raw_taxonomy
            if taxonomy_map.get(value, value)
        }
    )
    raw_score = nested_value(record, fields.get("score"))
    raw_adoption = nested_value(record, fields.get("adoption"), {})
    score_scale = mapping.get("scoreScale")
    score_target = score_scale.get("target") if isinstance(score_scale, dict) else None
    score = normalized_score(raw_score, score_scale)

    return {
        "canonicalIdHint": slug(name),
        "deduplicationFingerprint": fingerprint(name, description),
        "name": name,
        "description": description,
        "status": "discovered",
        "roles": mapping.get("defaults", {}).get("roles", []),
        "taskCategories": mapping.get("defaults", {}).get("taskCategories", []),
        "keywords": normalized_tags,
        "sourceEvidence": {
            "sourceId": source_id,
            "originalId": original_id,
            "originalTaxonomy": raw_taxonomy,
            "originalScore": {
                "value": raw_score,
                "scale": score_scale,
                "normalizedTarget": score_target,
                "normalizedValue": score,
            },
            "adoption": raw_adoption if isinstance(raw_adoption, dict) else {"value": raw_adoption},
        },
        "reviewNotes": [
            "Candidate requires deduplication and human review.",
            "Normalised scores are not comparable without reviewing source evidence.",
            "Skill files, source, licence, capabilities, and review metadata are incomplete.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--source-id", required=True)
    parser.add_argument("--mapping", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()

    try:
        inventory = json.loads(args.input.read_text(encoding="utf-8"))
        mapping = json.loads(args.mapping.read_text(encoding="utf-8"))
        if not isinstance(mapping, dict) or not isinstance(mapping.get("fields"), dict):
            raise ValueError("mapping must contain a fields object")
        records = records_from(inventory, mapping.get("recordsPath"))
        candidates = [
            normalize_record(record, index, args.source_id, mapping)
            for index, record in enumerate(records)
        ]
        result = {
            "schemaVersion": "1.0.0",
            "sourceId": args.source_id,
            "candidateCount": len(candidates),
            "candidates": candidates,
        }
        rendered = json.dumps(result, indent=2) + "\n"
        if args.output:
            if not args.apply:
                print(
                    json.dumps(
                        {
                            "operation": "write-normalized-candidates",
                            "output": str(args.output),
                            "candidateCount": len(candidates),
                            "applied": False,
                        },
                        indent=2,
                    )
                )
                return 0
            output = args.output.expanduser()
            if not output.is_absolute():
                output = Path.cwd() / output
            if output.exists() and output.is_symlink():
                raise ValueError("output must not be a symbolic link")
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(rendered, encoding="utf-8")
            print(
                json.dumps(
                    {
                        "operation": "write-normalized-candidates",
                        "output": str(output),
                        "candidateCount": len(candidates),
                        "applied": True,
                    },
                    indent=2,
                )
            )
        else:
            print(rendered, end="")
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Inventory normalization error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
