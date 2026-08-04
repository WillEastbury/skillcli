"""Deterministic helper for the Prompt Quality Check skill."""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class Check:
    name: str
    passed: bool
    guidance: str


def assess_prompt(prompt: str) -> dict[str, object]:
    """Assess basic prompt completeness without external services."""
    if not isinstance(prompt, str) or not prompt.strip():
        raise ValueError("prompt must be a non-empty string")

    normalized = prompt.strip()
    lowered = normalized.lower()
    checks = [
        Check(
            "objective",
            any(token in lowered for token in ("create", "write", "review", "explain", "build", "analyse", "analyze")),
            "State the action or outcome the agent should produce.",
        ),
        Check(
            "context",
            len(normalized.split()) >= 12,
            "Add the minimum background needed to make the request unambiguous.",
        ),
        Check(
            "constraints",
            any(token in lowered for token in ("must", "should", "do not", "only", "limit", "avoid")),
            "Declare important boundaries, exclusions, or limits.",
        ),
        Check(
            "output_shape",
            any(token in lowered for token in ("format", "table", "json", "markdown", "list", "paragraph")),
            "Specify the expected response structure.",
        ),
        Check(
            "acceptance_criteria",
            any(token in lowered for token in ("success", "complete", "verify", "test", "acceptance")),
            "Explain how a complete or correct result will be judged.",
        ),
        Check(
            "sensitive_data",
            not any(token in lowered for token in ("password", "api key", "access token", "secret key")),
            "Remove secrets and replace them with named placeholders.",
        ),
    ]

    passed = sum(check.passed for check in checks)
    return {
        "score": round(passed / len(checks) * 100),
        "checks": [asdict(check) for check in checks],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prompt", help="Prompt text to assess")
    args = parser.parse_args()
    print(json.dumps(assess_prompt(args.prompt), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
