---
name: "Prompt Quality Check"
description: "Reviews a prompt for clarity, context, constraints, output shape, and safety before producing an improved version."
version: "1.0.1"
skill-id: "prompt-quality-check"
---

# Prompt Quality Check

## Purpose

Review a user-provided prompt before it is sent to another AI system. Identify missing
context, ambiguous instructions, unsafe assumptions, and unclear output requirements,
then produce a stronger prompt without changing the user's intended outcome.

## When to use

Use this skill when a user asks to improve, review, harden, or clarify a prompt.

Do not use it to silently alter a prompt while performing an unrelated task.

## Inputs

- The original prompt.
- Optional audience, system, constraints, examples, and desired output format.

## Procedure

1. Restate the intended outcome in one sentence.
2. Assess these dimensions:
   - objective clarity
   - relevant context
   - constraints and boundaries
   - expected output shape
   - acceptance criteria
   - safety and sensitive-data handling
3. Distinguish blocking gaps from optional improvements.
4. Preserve explicit user constraints and terminology.
5. Write an improved prompt using only supplied facts. Put unresolved assumptions in a
   clearly labelled placeholder rather than inventing details.
6. Do not include secrets, credentials, or unnecessary personal data.

## Output

Return:

```text
Assessment
- Strengths:
- Blocking gaps:
- Optional improvements:

Improved prompt
<rewritten prompt>

Assumptions to confirm
- <only unresolved assumptions; "None" when complete>
```

## Optional source

`src/prompt_quality.py` is a standard-library reference helper for deterministic checks.
It has no network, filesystem, shell, secret, or third-party dependency requirements.
Do not execute it merely to scan or import this skill.
